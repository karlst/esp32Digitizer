// Copyright (C) 2026 Karl Stock
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file SdChokeTest.cpp
 * Timed generated input, circular SD writes, and retained-file readback.
 * Core 1 generate() offers 30,720 bytes every 10 ms (3,072,000 bytes/second).
 * Core 0 run() calls FileRecordingBackend::pump(), which drains its owned ring
 * and writes the selected pool. The test owns only generation and verification.
 * The numbered synthetic stream measures storage integrity, not ADC behavior.
 * Add-files snapshots old files before preparation and checks them after recording.
 */
#include "SdChokeTest.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <inttypes.h>
#include <ctime>
#include <sys/time.h>

/* **
 * clockUs - Monotonic microseconds used to pace input and measure storage
 * stalls.
 */
uint64_t SdChokeTest::clockUs()
{
    return static_cast<uint64_t>(esp_timer_get_time());
}

/* **
 * allocateRing - Allocate the writer's owned ring in PSRAM; null means
 * insufficient memory.
 */
uint8_t* SdChokeTest::allocateRing(uint32_t bytes)
{
    return static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

/* **
 * releaseRing - Release the writer-owned PSRAM allocation when its owner is
 * destroyed.
 */
void SdChokeTest::releaseRing(uint8_t* memory)
{
    heap_caps_free(memory);
}

/* **
 * start - Start the selected SD test if idle and UTC has been set. Create a disk
 * task on Core 0, which performs preparation and recording later. Return true
 * when that task was created, not when the card is ready. Reject another start
 * while this test is using the card.
 */
bool SdChokeTest::start(Mode mode)
{
    State expected = State::idle;
    bool retVal = _utcSet && _state.compare_exchange_strong(expected, State::preparing);
    if (retVal)
    {
        _mode = mode;
        retVal = xTaskCreatePinnedToCore(diskEntry, "sdChokeDisk", _diskStackBytes, this, 2, nullptr, 0) == pdPASS;
        if (!retVal)
        {
            _failure = "cannot create disk task";
            _state.store(State::done);
        }
    }
    return retVal;
}

/* **
 * setUtc - Console supplies PC UTC seconds since 1970, before any test starts.
 * Reject dates outside 2020–2099 and clock changes after preparation begins.
 * settimeofday sets calendar time; input pacing still uses the monotonic timer.
 * Accuracy depends on the PC clock and serial delivery, not a precision time
 * source.
 */
bool SdChokeTest::setUtc(uint64_t seconds)
{
    bool retVal = _state.load() == State::idle && seconds >= 1577836800ULL && seconds < 4102444800ULL;
    if (retVal)
    {
        timeval utc = {};
        utc.tv_sec = static_cast<time_t>(seconds);
        retVal = settimeofday(&utc, nullptr) == 0;
        _utcSet = retVal;
    }
    return retVal;
}

/* **
 * identify - Identify idle timestamp-capable test firmware before the PC sends
 * an erase command.
 */
void SdChokeTest::identify() const
{
    printf("CHOKE version=5 idle=%d utc=%d\n", _state.load() == State::idle ? 1 : 0, _utcSet ? 1 : 0);
}

/* **
 * stop - Request generator stop; already queued data is saved before readback.
 */
void SdChokeTest::stop()
{
    const State state = _state.load();
    if (state == State::preparing || state == State::running)
    {
        _stopRequested.store(true);
    }
}

/* **
 * diskEntry - FreeRTOS disk-thread entry; run completes before this task deletes
 * itself.
 */
void SdChokeTest::diskEntry(void* context)
{
    static_cast<SdChokeTest*>(context)->run();
    vTaskDelete(nullptr);
}

/* **
 * producerEntry - FreeRTOS generator entry; signal completion before deleting
 * this task.
 */
void SdChokeTest::producerEntry(void* context)
{
    static_cast<SdChokeTest*>(context)->generate();
    vTaskDelete(nullptr);
}

/* **
 * fillPattern - Fill data with consecutive eight-byte numbers, starting at
 * offset / 8. offset and length are byte counts and must be multiples of eight.
 * The P4 stores the lowest byte first. memcpy permits copying even when data is
 * not aligned for a 64-bit integer.
 *
 * Number the bytes accepted by the recorder; record rejected or late input
 * separately in the loss counters.
 */
void SdChokeTest::fillPattern(uint8_t* data, uint32_t length, uint64_t offset)
{
    for (uint32_t iByte = 0; iByte < length; iByte += 8)
    {
        const uint64_t word = (offset + iByte) / 8;
        memcpy(data + iByte, &word, sizeof(word));
    }
}

/* **
 * generate - Offer generated data to the recorder on Core 1 at a fixed rate.
 * Each 10 ms interval represents one batch. If several intervals were missed,
 * count the older batches as lost and offer only the newest batch.
 *
 * submit copies into the ring without waiting for SD writes. Do not retry
 * rejected data. Briefly lock only the shared progress counters so the console
 * can read a consistent report; never hold that lock while copying data or
 * writing to disk.
 */
void SdChokeTest::generate()
{
    const uint64_t began = clockUs();
    const uint64_t batches = uint64_t(_runLimitSeconds) * 100;
    uint64_t accounted = 0, accepted = 0, lost = 0, late = 0;
    while (accounted < batches && !_stopRequested.load() && !_fileRecordingBackend.getFailed() && !_fileRecordingBackend.getFull())
    {
        const uint64_t due = std::min(batches, (clockUs() - began) / 10000);
        if (due > accounted)
        {
            // Only the newest overdue batch is offered. Track skipped deadlines
            // separately from real full-ring rejections; both fail a no-loss run.
            const uint64_t skipped = (due - accounted - 1) * _batchBytes;
            late += skipped;
            lost += skipped;
            _fileRecordingBackend.skipInput(skipped);
            fillPattern(_generatedData, _batchBytes, accepted);
            const uint32_t saved = _fileRecordingBackend.copyToRingBuffer(_generatedData, _batchBytes);
            accepted += saved;
            lost += _batchBytes - saved;
            accounted = due;

            // getQueuedBytes is safe here because this is the sole producer.
            // A third task must use this snapshot, not read ring positions itself.
            const uint32_t used = _fileRecordingBackend.getQueuedBytes();
            portENTER_CRITICAL(&_snapshotMux);
            _snapshot._scheduled = accounted * _batchBytes;
            _snapshot._accepted = accepted;
            _snapshot._lost = lost;
            _snapshot._late = late;
            _snapshot._seconds = static_cast<uint32_t>((clockUs() - began) / 1000000);
            _snapshot._used = used;
            _snapshot._peak = std::max(_snapshot._peak, used);
            portEXIT_CRITICAL(&_snapshotMux);
        }

        // One scheduler tick lets idle/system tasks run; the choke build uses
        // 1000 ticks/second. This is a workload generator, not an ADC simulator.
        vTaskDelay(1);
    }
    _producerDone.store(true, std::memory_order_release);
}

/* **
 * utcSeconds - Calendar time for file headers; PC sets this before preparation.
 */
uint64_t SdChokeTest::utcSeconds()
{
    return static_cast<uint64_t>(time(nullptr));
}

/* **
 * checkPattern - Check synthetic little-endian word numbers, including split
 * words. Backend reads can start/end between eight-byte words. The callback
 * never keeps its scratch pointer. Yielding here cannot affect the completed
 * timed run.
 */
bool SdChokeTest::checkPattern(const uint8_t* data, uint32_t length, uint64_t offset)
{
    bool retVal = true;
    for (uint32_t iByte = 0; retVal && iByte < length; ++iByte)
    {
        const uint64_t position = offset + iByte;
        retVal = data[iByte] == static_cast<uint8_t>((position / 8) >> (8 * (position % 8)));
    }
    vTaskDelay(1);
    return retVal;
}

/* **
 * updateStorageSnapshot - Copy disk statistics into the shared progress report
 * under a short lock. Called by the disk task. The console reads this copy
 * instead of accessing backend statistics while they are changing.
 */
void SdChokeTest::updateStorageSnapshot()
{
    const auto statistics = _fileRecordingBackend.getStatistics();
    portENTER_CRITICAL(&_snapshotMux);
    _snapshot._written = statistics._written;
    _snapshot._writeUs = statistics._writeUs;
    _snapshot._rolloverUs = statistics._rolloverUs;
    _snapshot._fileNumber = statistics._fileNumber;
    _snapshot._rollovers = statistics._rollovers;
    _snapshot._peak = std::max(_snapshot._peak, statistics._buffer._peak);
    _snapshot._bufferHoldUs = statistics._buffer._maxUs;
    portEXIT_CRITICAL(&_snapshotMux);
}

/* **
 * expectUsedPoolRefusal - Check that useExisting with stopWhenFull refuses to
 * overwrite completed recordings. First confirm the card has at least one
 * compatible completed file. Then require the specific no-available-files error.
 *
 * A different error is a test failure, not evidence that overwrite protection
 * works. If preparation unexpectedly succeeds, finish that empty session and
 * fail the check.
 */
bool SdChokeTest::expectUsedPoolRefusal(RecordingSessionConfig& config)
{
    config._preparation = RecordingSessionConfig::Preparation::useExisting;
    config._fullPolicy = RecordingSessionConfig::FullPolicy::stopWhenFull;
    // Require an actual compatible used file. An empty/missing pool or a card
    // with unrelated data is not evidence that overwrite protection worked.
    FF_DIR directory = {};
    const std::string path = std::string(_p4RecordingDisk.getDrive()) + "/RECORD";
    bool readable = f_opendir(&directory, path.c_str()) == FR_OK;
    const bool opened = readable;
    bool ended = false, foundUsed = false;
    while (readable && !ended)
    {
        FILINFO info = {};
        readable = f_readdir(&directory, &info) == FR_OK;
        ended = !readable || info.fname[0] == '\0';
        if (!ended && !(info.fattrib & AM_DIR) && info.fsize >= 512)
        {
            // Reuse the object's file handle; close it before the next file.
            readable = f_open(&_inspectionFile, (path + "/" + info.fname).c_str(), FA_READ) == FR_OK;
            if (readable)
            {
                uint8_t bytes[512];
                UINT actual = 0;
                readable = f_read(&_inspectionFile, bytes, sizeof(bytes), &actual) == FR_OK && actual == sizeof(bytes);
                SessionFileHeader sessionFileHeader;
                if (readable && sessionFileHeader.decode(bytes) && sessionFileHeader._state == 2 &&
                    sessionFileHeader._sampleRate == config._sampleRate && sessionFileHeader._bytesPerSample == config._bytesPerSample &&
                    sessionFileHeader._sampleFormat == config._sampleFormat && sessionFileHeader._validBytes > 0)
                {
                    foundUsed = true;
                }
                const bool closed = f_close(&_inspectionFile) == FR_OK;
                readable = readable && closed;
            }
        }
    }
    if (opened)
    {
        const bool closed = f_closedir(&directory) == FR_OK;
        readable = readable && closed;
    }
    if (!readable || !foundUsed)
    {
        _failure = "no completed compatible recording; run fresh/add-files first";
        return false;
    }
    const bool prepared = _fileRecordingBackend.prepForRecording(_p4RecordingDisk, config);
    const bool retVal = !prepared && strcmp(_fileRecordingBackend.getError(), "no compatible available prepared files") == 0;
    if (prepared)
    {
        _fileRecordingBackend.finishRecording();
    }
    if (!retVal)
    {
        _failure = "expected used-pool refusal; run fresh/add-files to completion first";
    }
    return retVal;
}

/* **
 * prepareExistingTest - Establish five unused files before the measured
 * useExisting run. First reject the already used pool, then reserve additional
 * files without starting a session. Snapshot all filenames/sizes after
 * allocation. The later prepare(useExisting) must select those slots without
 * allocating or resizing. The original-file checksum baseline was captured
 * before this setup.
 */
bool SdChokeTest::prepareExistingTest(RecordingSessionConfig& config)
{
    _usedRefused = expectUsedPoolRefusal(config);
    bool retVal = _usedRefused;
    if (retVal)
    {
        config._preparation = RecordingSessionConfig::Preparation::addFiles;
        _failure = "preallocating unused test files";
        retVal = _fileRecordingBackend.preallocate(_p4RecordingDisk, config);
    }
    if (retVal)
    {
        _failure = "capturing prepared file names/sizes";
        _layoutReady = _preservedLayout.capture(_p4RecordingDisk.getDrive(), false);
        retVal = _layoutReady;
    }
    config._preparation = RecordingSessionConfig::Preparation::useExisting;
    // Allow a short timeout margin, but require the full flag and exact capacity.
    // The generator stops on the backend's full flag, not on the nominal 75 seconds.
    _runLimitSeconds = _fileCount * _secondsPerFile + 5;
    return retVal;
}

/* **
 * run - Disk task prepares a session, runs generation, drains, then verifies.
 * Only the fresh selection formats. Add-files checks every old file before and
 * after, outside the timed interval. Release/acquire on producerDone prevents
 * submission racing finish. No Feather, ADC or console service belongs in
 * backend.
 */
void SdChokeTest::run()
{
    _generatedData = static_cast<uint8_t*>(heap_caps_malloc(_batchBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    bool ok = _generatedData && _fileRecordingBackend.initializeMemory(allocateRing, releaseRing, clockUs, utcSeconds);
    bool baselineReady = false;
    _failure = "memory allocation";
    if (ok && _mode != Mode::fresh)
    {
        _failure = "old-file baseline failed (run fresh first on an empty card)";
        ok = _p4RecordingDisk.prepare(false) && _preservedFiles.capture(_p4RecordingDisk.getDrive());
        baselineReady = ok;
    }

    // Config describes physical payload capacity; header bytes are added by backend.
    RecordingSessionConfig recordingSessionConfig;
    recordingSessionConfig._preparation = _mode != Mode::fresh ? RecordingSessionConfig::Preparation::addFiles : RecordingSessionConfig::Preparation::formatAndAllocate;
    recordingSessionConfig._fullPolicy = RecordingSessionConfig::FullPolicy::overwriteOldest;
    recordingSessionConfig._secondsPerFile = _secondsPerFile;
    recordingSessionConfig._fileCount = _fileCount;
    recordingSessionConfig._sampleRate = _bytesPerSecond / 3;
    recordingSessionConfig._bytesPerSample = 3;
    recordingSessionConfig._sampleFormat = 2; // Synthetic byte stream, not signed ADC values.
    if (ok && _mode == Mode::existing)
    {
        ok = prepareExistingTest(recordingSessionConfig);
    }
    if (ok)
    {
        ok = _fileRecordingBackend.prepForRecording(_p4RecordingDisk, recordingSessionConfig);
        if (!ok)
        {
            _failure = _fileRecordingBackend.getError();
        }
    }
    if (ok && _mode == Mode::existing)
    {
        _failure = "existing pool selection/layout changed";
        ok = _fileRecordingBackend.getStatistics()._files == _fileCount && _preservedLayout.verify();
        if (!ok)
        {
            _fileRecordingBackend.finishRecording();
        }
    }
    if (ok)
    {
        _failure = "creating generator task";
        _state.store(State::running);
        ok = xTaskCreatePinnedToCore(producerEntry, "sdChokeInput", 4096, this, 3, nullptr, 1) == pdPASS;
        if (ok)
        {
            while (!_producerDone.load(std::memory_order_acquire))
            {
                if (ok)
                {
                    ok = _fileRecordingBackend.pump();
                }
                if (!ok)
                {
                    _stopRequested.store(true);
                }
                updateStorageSnapshot();
                vTaskDelay(1);
            }
        }
        _state.store(State::draining);
        const bool finished = _fileRecordingBackend.finishRecording();
        if (!finished)
        {
            _failure = _fileRecordingBackend.getError();
        }
        ok = ok && finished;
        updateStorageSnapshot();
        if (ok)
        {
            _failure = "new-file header/payload readback";
            _state.store(State::verifying);
            _readback = _fileRecordingBackend.verify(checkPattern);
            ok = _readback;
        }
        if (ok)
        {
            _failure = "lost input, stopped early, or incomplete write";
            ok = !_stopRequested.load() && _snapshot._lost == 0 &&
                 _snapshot._scheduled == (_mode == Mode::existing ? _fileBytes * _fileCount : uint64_t(_runSeconds) * _bytesPerSecond) &&
                 _snapshot._accepted == _snapshot._written;
        }
    }
    if (_layoutReady)
    {
        _layoutUnchanged = _preservedLayout.verify();
        if (!_layoutUnchanged)
        {
            _failure = "a file was added, removed or resized during use-existing";
        }
        ok = ok && _layoutUnchanged;
        if (ok)
        {
            _failure = "pool did not stop cleanly at capacity";
            ok = _fileRecordingBackend.getFull() && _snapshot._rollovers == _fileCount - 1 &&
                 _snapshot._written == _fileBytes * _fileCount;
        }
        if (ok)
        {
            _fullRefused = expectUsedPoolRefusal(recordingSessionConfig);
            ok = _fullRefused;
        }
    }
    if (baselineReady)
    {
        // Even failed preparation/writing must not exempt old files from checking.
        _state.store(State::preserving);
        _preserved = _preservedFiles.verify();
        if (!_preserved)
        {
            _failure = "an old file changed or could not be read";
        }
        ok = ok && _preserved;
    }
    heap_caps_free(_generatedData);
    _generatedData = nullptr;
    _success = ok;
    if (ok)
    {
        _failure = "none";
    }

    // ESP-IDF reports the minimum unused stack in bytes over this task's life.
    // This measures this run only; it is not a guarantee for every future path.
    printf("Disk task minimum unused stack: %u bytes\n", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    _state.store(State::done, std::memory_order_release);
}

/* **
 * report - Console task prints short progress and final results from a locked
 * copy. No printing happens while holding the lock. The done state publishes
 * final fields after the disk task stops changing them. Progress is
 * informational; final peak comes from the writer's actual submission high-water
 * mark.
 */
void SdChokeTest::report()
{
    const State state = _state.load(std::memory_order_acquire);
    Snapshot snapshot;
    portENTER_CRITICAL(&_snapshotMux);
    snapshot = _snapshot;
    portEXIT_CRITICAL(&_snapshotMux);
    if (state != _lastReportedState)
    {
        if (state == State::preparing)
        {
            printf("%s; reserving %u files x %u seconds (%" PRIu64 " payload bytes each).\n",
                _mode == Mode::fresh ? "Formatting SD" : "Checking old files, then preparing test", unsigned(_fileCount), unsigned(_secondsPerFile), _fileBytes);
        }
        else if (state == State::running)
        {
            printf("Running %u seconds at %u bytes/s; ring %u MiB.\n",
                unsigned(_mode == Mode::existing ? _fileCount * _secondsPerFile : _runSeconds), unsigned(_bytesPerSecond), unsigned(BufferedWriter::_capacity / 1048576));
        }
        else if (state == State::draining)
        {
            printf("Saving remaining buffered data...\n");
        }
        else if (state == State::verifying)
        {
            printf("Reading retained files back...\n");
        }
        else if (state == State::preserving)
        {
            printf("Checking original files are unchanged...\n");
        }
        _lastReportedState = state;
    }
#ifdef CONFIG_P4_CHOKE_LIVE_REPORT
    if (state == State::running)
    {
        printf("Elapsed %us | File %u/%u | Buffer %.1f%% | Peak %.1f%% | Lost %" PRIu64 " bytes\n",
            unsigned(snapshot._seconds), unsigned(snapshot._fileNumber), unsigned(_fileCount),
            100.0 * snapshot._used / BufferedWriter::_capacity,
            100.0 * snapshot._peak / BufferedWriter::_capacity, snapshot._lost);
    }
#endif
    if (state == State::done && !_reported)
    {
        printf("Scheduled %" PRIu64 " | Accepted %" PRIu64 " | Written %" PRIu64 " | Lost %" PRIu64 " bytes\n",
            snapshot._scheduled, snapshot._accepted, snapshot._written, snapshot._lost);
        printf("Loss: buffer/rejection %" PRIu64 " | generator late %" PRIu64 " bytes\n",
            snapshot._lost - snapshot._late, snapshot._late);
        printf("Peak buffer %.1f%% | Longest write %.1f ms | Rollover %.1f ms | Changes %u | Reuses %u\n",
            100.0 * snapshot._peak / BufferedWriter::_capacity, snapshot._writeUs / 1000.0,
            snapshot._rolloverUs / 1000.0, unsigned(snapshot._rollovers),
            snapshot._rollovers >= _fileCount ? unsigned(snapshot._rollovers - _fileCount + 1) : 0u);
        printf("Longest buffered write/flush: %.1f ms (includes any rollover within that write).\n",
            snapshot._bufferHoldUs / 1000.0);
        printf("New-file headers/payload: %s\n", _readback ? "match" : "not verified");
        if (_mode != Mode::fresh)
        {
            printf("Original files: %u | %s\n", unsigned(_preservedFiles.getCount()),
                _preserved ? "unchanged" : "not verified unchanged");
        }
        if (_mode == Mode::existing)
        {
            printf("Used pool refused: %s | Full pool refused: %s | File names/sizes unchanged: %s\n",
                _usedRefused ? "yes" : "no", _fullRefused ? "yes" : "no", _layoutUnchanged ? "yes" : "no");
        }
        printf("%s: %s\n", _success ? "PASS" : "FAIL",
            _success ? (_mode == Mode::existing ? "stopped at capacity; no loss; original files unchanged" : "full timed run; no loss; file checks passed") : _failure);
        _reported = true;
    }
}
