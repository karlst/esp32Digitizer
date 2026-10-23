/*
 * Copyright (C) 2026 Karl Stock
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of ESP32 Digitizer.
 * You may redistribute and/or modify it under the GNU General Public License
 * as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 *
 * Distributed WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License in the repository LICENSE file or <https://www.gnu.org/licenses/>.
 */

/**
 * @file SdRecorder.cpp
 * Connect decoded ADC samples to the reusable SD-writing backend. Core 1 packs
 * and submits samples. A separate disk task on Core 0 prepares files, writes
 * queued bytes and verifies retained data after recording stops. Read initTask,
 * prepare, submitSamples, work and finishFile in that order. Atomic state flags
 * coordinate the tasks so the disk task sees complete requests and final
 * submission counts.
 */
#include "SdRecorder.h"
#include <cstdio>
#include <ctime>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"

/* **
 * SdRecorder - Save the session settings and optional reporting objects. The
 * caller owns the reporters and must keep them alive while this recorder uses
 * them. Construction does not allocate the ring or access the SD card.
 */
SdRecorder::SdRecorder(const RecordingSessionConfig& recordingSessionConfig,
    RecordingReadback* recordingReadback, RecordingReporter* recordingReporter)
    : _recordingReporter(recordingReporter), _recordingReadback(recordingReadback),
      _recordingSessionConfig(recordingSessionConfig)
{
}

/* **
 * allocateRing - Allocate bytes of ring-buffer memory in PSRAM, the P4's
 * external RAM. Return its address, or null if there is not enough memory.
 */
static uint8_t* allocateRing(uint32_t bytes)
{
    return static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

/* **
 * releaseRing - Free ring memory previously returned by allocateRing.
 * BufferedWriter saves this function and calls it when the writer is destroyed;
 * this is not how space is released after an individual write.
 */
static void releaseRing(uint8_t* memory)
{
    heap_caps_free(memory);
}

/* **
 * recordingClock - Return elapsed microseconds from the ESP timer. Disk-write
 * timing uses this clock so changing the calendar time cannot change measured
 * write durations.
 */
static uint64_t recordingClock()
{
    return static_cast<uint64_t>(esp_timer_get_time());
}

/* **
 * recordingUtc - Return calendar time as seconds since 1970 in UTC. File headers
 * use this value. The backend refuses preparation if the clock has not been set
 * to a supported date.
 */
static uint64_t recordingUtc()
{
    return static_cast<uint64_t>(time(nullptr));
}

/* **
 * initTask - Allocate recording buffers and start the disk task on Core 0.
 * Startup calls this once. A task is a software thread scheduled by FreeRTOS.
 * This task has a 16 KB stack for its calls and local variables, separate from
 * the 16 MB ring buffer.
 *
 * Return true when memory and task creation succeed. The task waits for
 * preparation requests; success here does not mean the SD card is ready.
 */
bool SdRecorder::initTask()
{
    const bool retVal = _fileRecordingBackend.initializeMemory(allocateRing, releaseRing, recordingClock, recordingUtc) &&
                        xTaskCreatePinnedToCore(taskEntry, "sdWriter", 16384, this, 4, &_diskTask, 0) == pdPASS;
    return retVal;
}

/* **
 * configure - Save new file-pool settings while the recorder is idle. Return
 * false after preparation has begun. This changes the next preparation request;
 * it does not access the card.
 */
bool SdRecorder::configure(const RecordingSessionConfig& config)
{
    const bool retVal = _state.load() == Phase::idle;
    if (retVal)
    {
        _recordingSessionConfig = config;
    }
    return retVal;
}

/* **
 * getTaskHandle - Return the disk task's identifier for optional timing
 * diagnostics. The caller must not delete or take over the task.
 */
TaskHandle_t SdRecorder::getTaskHandle() const
{
    return _diskTask;
}

/* **
 * prepare - Ask the disk task to prepare a recording. run identifies the
 * acquisition in reports; rate is samples per second. recordingLayout describes
 * the sample byte format and optional test checks, and must remain available
 * until completion.
 *
 * Return true if the request was accepted. Preparation happens later in work;
 * getReady confirms it completed. Save the arguments before setting the atomic
 * state so the disk task sees a complete request.
 */
bool SdRecorder::prepare(uint32_t run, uint32_t rate, const RecordingLayout* recordingLayout)
{
    const Phase current = _state.load();
    const bool retVal = _diskTask && (current == Phase::idle || current == Phase::complete);
    if (retVal)
    {
        _recordingLayout = recordingLayout ? recordingLayout : &_recordingLayoutDefault;
        _runId = run;
        _sampleRate = rate;
        _preparationCancelled.store(false);
        _state.store(Phase::preparing);
    }
    return retVal;
}

/* **
 * getReady - Return true after disk preparation has completed, unless
 * cancellation was requested. This means data can be submitted; it does not mean
 * the ADC is running.
 */
bool SdRecorder::getReady() const
{
    return _state.load() == Phase::running && !_preparationCancelled.load();
}

/* **
 * cancelPreparation - Ask the disk task to cancel preparation before sample
 * submission starts. This only sets a flag; the disk task finishes any pending
 * operation and closes files.
 */
void SdRecorder::cancelPreparation()
{
    _preparationCancelled.store(true);
}

/* **
 * closeCancelledPreparation - Close a prepared session that was cancelled before
 * receiving samples. Keep its allocated files. Return the recorder to idle only
 * if cleanup succeeds; otherwise block reuse until reset.
 */
void SdRecorder::closeCancelledPreparation()
{
    const bool closed = _fileRecordingBackend.finishRecording();
    _state.store(closed ? Phase::idle : Phase::blocked);
}

/* **
 * submitSamples - Convert up to 512 decoded ADC words into three-byte samples,
 * then submit them to the ring buffer. Core 1 calls this after FrameDecoder
 * checks the status bits. For example, 0x08123456 becomes bytes 56 34 12.
 *
 * Return true only if the whole batch was packed and accepted. This does not
 * write to SD. submitBytes copies the packed bytes before the temporary packing
 * array is reused.
 */
bool SdRecorder::submitSamples(const uint32_t* words, size_t count)
{
    const size_t bytes = recordingFormat::packSamples(words, count, _packedSamples, sizeof(_packedSamples));
    bool retVal = count > 0 && count <= 512 && bytes == count * 3;
    if (retVal)
    {
        retVal = submitBytes(_packedSamples, static_cast<uint32_t>(bytes));
    }
    else
    {
        _storageBroken.store(true);
    }
    return retVal;
}

/* **
 * submitBytes - Copy a batch into the backend ring and update checksums for the
 * accepted bytes. Called on Core 1; no disk operation runs here. Return true
 * only if the complete nonempty batch was accepted.
 *
 * A short acceptance marks this acquisition as failed. Do not retry the batch,
 * because some bytes may already be queued. The disk task reads the submission
 * totals only after decoding has stopped.
 */
bool SdRecorder::submitBytes(const uint8_t* data, uint32_t bytes)
{
    const uint32_t accepted = getReady() ? _fileRecordingBackend.copyToRingBuffer(data, bytes) : 0;
    if (accepted)
    {
        _submittedBytes += accepted;
        _retainedPayloadChecksums.accept(data, accepted);
    }
    const bool retVal = bytes != 0 && accepted == bytes;
    if (!retVal)
    {
        _storageBroken.store(true);
    }
    return retVal;
}

/* **
 * getFailed - Return true after preparation, writing or submission fails. This
 * reads shared flags rather than accessing the disk task's changing statistics.
 */
bool SdRecorder::getFailed() const
{
    const Phase current = _state.load();
    return current == Phase::error || current == Phase::blocked ||
           ((current == Phase::running || current == Phase::finishing || current == Phase::complete) &&
               (_fileRecordingBackend.getFailed() || _storageBroken.load()));
}

/* **
 * getFull - Return true when a running stopWhenFull session has exhausted its
 * selected file capacity. The controller uses this to request source shutdown.
 * It does not report ring-buffer occupancy.
 */
bool SdRecorder::getFull() const
{
    return _state.load() == Phase::running && _fileRecordingBackend.getFull();
}

/* **
 * finish - Ask the disk task to finish after all decoder submissions have
 * stopped. captureClean is the acquisition's result; expectedSamples must match
 * the number of bytes accepted.
 *
 * The atomic state change makes the final submission counts available to the
 * disk task. This returns before the ring is drained or files are verified;
 * finishFile does that work.
 */
void SdRecorder::finish(bool captureClean, uint32_t expectedSamples)
{
    _clean = captureClean && _recordingLayout->countMatches(_submittedBytes, expectedSamples);
    _state.store(Phase::finishing);
}

/* **
 * taskEntry - Start the recorder's disk-service loop. FreeRTOS supplies the same
 * SdRecorder pointer passed when the task was created. The recorder must remain
 * alive for the task's lifetime.
 */
void SdRecorder::taskEntry(void* context)
{
    static_cast<SdRecorder*>(context)->work();
}

/* **
 * work - Run the disk task on Core 0. Prepare files when requested, pump queued
 * bytes during recording, and finish and verify files after submission stops.
 *
 * Sleep one scheduler tick between passes so command handling can run. An
 * unrecoverable storage failure prevents another session until reset. This loop
 * does not automatically format the card or retry failed writes.
 */
void SdRecorder::work()
{
    for (;;)
    {
        const Phase current = _state.load();
        if (current == Phase::preparing)
        {
            const bool prepared = prepareFile();
            if (prepared && _preparationCancelled.load())
            {
                closeCancelledPreparation();
            }
            else
            {
                if (_recordingReporter)
                {
                    _recordingReporter->storagePrepared(prepared, _path);
                }
                _state.store(prepared ? Phase::running : Phase::error);
            }
        }
        else if (current == Phase::running)
        {
            if (_preparationCancelled.load())
            {
                closeCancelledPreparation();
            }
            else
            {
                _fileRecordingBackend.pump();
            }
        }
        else if (current == Phase::finishing)
        {
            finishFile();
        }
        vTaskDelay(1);
    }
}

/* **
 * prepareFile - Prepare the configured file pool on the disk task. Pass the
 * sample rate and byte format to FileRecordingBackend, then prepare checksums
 * for its selected file capacities.
 *
 * The disk session number is separate from Feather's acquisition number. File
 * timestamps come from the UTC clock. Return true when the pool is ready; ADC
 * receiver setup still has to complete before acquisition starts.
 */
bool SdRecorder::prepareFile()
{
    _fileRecordingBackend.setProgressReporter(reportProgress, this);
    reportProgress(this, "disk task started preparation");
    _recordingSessionConfig._sampleRate = _sampleRate;
    _recordingSessionConfig._bytesPerSample = _recordingLayout->getRecordBytes();
    _recordingSessionConfig._sampleFormat = _recordingLayout->getRecordBytes() == 3 ? 1 : 2;
    _submittedBytes = 0;
    _clean = false;
    _storageBroken.store(false);
    snprintf(_path, sizeof(_path), "RECORD/pending");
    bool retVal = _fileRecordingBackend.prepForRecording(_p4RecordingDisk, _recordingSessionConfig);
    if (retVal)
    {
        retVal = _retainedPayloadChecksums.prepare(_fileRecordingBackend.getFileCapacities(), esp_rom_crc32_le);
        if (!retVal)
        {
            _fileRecordingBackend.finishRecording();
        }
    }
    if (retVal)
    {
        const auto statistics = _fileRecordingBackend.getStatistics();
        snprintf(_path, sizeof(_path), "%s/RECORD/session-%llu", _p4RecordingDisk.getDrive(),
            static_cast<unsigned long long>(statistics._session));
        reportProgress(this, "file pool ready; receiver setup still required");
    }
    else if (_recordingReporter)
    {
        char message[192];
        snprintf(message, sizeof(message), "PREP FAILED: %s\n", _fileRecordingBackend.getError());
        _recordingReporter->message(message);
    }
    return retVal;
}

/* **
 * reportProgress - Send a disk-preparation message through the reporter.
 * The callback runs on the disk task before recording starts. Copy the temporary
 * text into one complete message, and do not retain the supplied pointer.
 */
void SdRecorder::reportProgress(void* context, const char* text)
{
    auto& sdRecorder = *static_cast<SdRecorder*>(context);
    if (sdRecorder._recordingReporter)
    {
        char message[192];
        snprintf(message, sizeof(message), "PREP: %s\n", text);
        sdRecorder._recordingReporter->message(message);
    }
}

/* **
 * checkReadback - Check one chunk read from SD after recording stops. context
 * identifies this recorder; offset is the byte position in the original session,
 * including any earlier data that was overwritten.
 *
 * Require continuous offsets and a matching checksum for each retained file.
 * Forward valid bytes to the optional waveform summary and format checker.
 * Return false at the first mismatch. Yield one scheduler tick after each chunk
 * so command handling remains responsive.
 */
bool SdRecorder::checkReadback(void* context, const uint8_t* data, uint32_t bytes, uint64_t offset)
{
    auto& sdRecorder = *static_cast<SdRecorder*>(context);
    const bool retVal = offset == sdRecorder._readbackBytes &&
                        sdRecorder._retainedPayloadChecksums.check(data, bytes, offset);
    if (retVal)
    {
        sdRecorder._readbackCrc = esp_rom_crc32_le(sdRecorder._readbackCrc, data, bytes);
        sdRecorder._patternErrors += sdRecorder._recordingLayout->check(data, bytes, offset);
        if (sdRecorder._recordingReadback && sdRecorder._recordingLayout->getRecordBytes() == 3)
        {
            sdRecorder._recordingReadback->accept(data, bytes);
        }
        sdRecorder._readbackBytes += bytes;
    }
    vTaskDelay(1);
    return retVal;
}

/* **
 * finishFile - Drain the ring, finalize the files and read the retained sample
 * data back. This runs on the disk task after Core 1 stops submitting.
 *
 * Report success only if storage operations, accepted/written byte counts,
 * retained-file checksums and acquisition checks all pass. Earlier overwritten
 * bytes cannot be verified. Headers marked complete describe storage completion,
 * not analog accuracy or a guarantee that every ADC sample arrived.
 */
void SdRecorder::finishFile()
{
    bool drained = true;
    while (_fileRecordingBackend.getQueuedBytes() && drained)
    {
        drained = _fileRecordingBackend.pump(true);
        vTaskDelay(1);
    }
    const bool finished = _fileRecordingBackend.finishRecording();
    const auto statistics = _fileRecordingBackend.getStatistics();
    if (_recordingReporter)
    {
        _recordingReporter->storageVerifying(statistics._written);
    }
    const uint64_t firstByte = _retainedPayloadChecksums.beginReadback();
    _readbackCrc = 0;
    _readbackBytes = firstByte;
    _patternErrors = 0;
    if (_recordingReadback && _recordingLayout->getRecordBytes() == 3)
    {
        _recordingReadback->beginRange(_sampleRate, firstByte / 3, _submittedBytes / 3);
    }
    const bool verified = finished && _fileRecordingBackend.verify(checkReadback, this);
    const bool ok = drained && finished && verified && _clean && !_storageBroken.load() &&
                    statistics._lostBytes == 0 && _submittedBytes != 0 && _submittedBytes == statistics._written &&
                    _readbackBytes == _submittedBytes && _retainedPayloadChecksums.getVerified() &&
                    _patternErrors == 0;

    // Keep transport formatting in the reporter. This metadata also lets the
    // smoke test require a real file boundary, rather than a single-file pass.
    if (_recordingReporter)
    {
        _recordingReporter->storageSession(statistics._session, statistics._files,
            statistics._rollovers, statistics._reuses, statistics._lostBytes);
        _recordingReporter->storageRetention(firstByte, _readbackBytes - firstByte, _submittedBytes);
        _recordingReporter->storageFinished(ok, _path, statistics._written, _readbackCrc, statistics._buffer, 0.0);
    }
    _recordingLayout->report(_patternErrors);
    if (_recordingReadback && _recordingLayout->getRecordBytes() == 3)
    {
        _recordingReadback->report(_runId, ok);
    }
    _state.store(ok ? Phase::complete : Phase::blocked);
}
