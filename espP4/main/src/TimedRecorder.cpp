// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Reading path: initialize creates the tasks; prepare requests a new file pool;
 * work prepares storage and enables format. Decoder submit copies packed samples
 * into staging, while addBoundary copies timing records. format consumes both
 * queues and fills the write ring; work drains that ring to the card. finish
 * requests shutdown; complete verifies the card only after format has stopped.
 * These are separate tasks (scheduled software threads), not interrupt handlers.
 */
#include "TimedRecorder.h"
#include "recordingFormat.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"
#include <cstdio>

/* **
 * allocateExternal - Allocate staging/write bytes in external PSRAM, not DMA memory.
 */
static uint8_t* allocateExternal(uint32_t bytes)
{
    return static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

/* **
 * allocateInternal - Match the passing FileRecordingBackend: internal DMA-capable
 * RAM aligned to 64 bytes. DMA is the SD controller's direct memory transfer.
 * Ordinary malloc alignment can force the SDK's slower copied-transfer path.
 */
static uint8_t* allocateInternal(uint32_t bytes)
{
    return static_cast<uint8_t*>(heap_caps_aligned_alloc(64, bytes,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
}

/* **
 * releaseMemory - Return an allocation made by either allocator above.
 */
static void releaseMemory(uint8_t* memory)
{
    heap_caps_free(memory);
}

/* **
 * timedClock - Monotonic microseconds measure SD write latency, not sample times.
 */
static uint64_t timedClock()
{
    return uint64_t(esp_timer_get_time());
}

/* **
 * TimedRecorder - Connect services without touching hardware or allocating memory.
 */
TimedRecorder::TimedRecorder()
    : _timedBufferedSink(_timedFatStorage), _timedRecordingStream(*this)
{
}

/* **
 * initialize - Called once before selecting this destination. Allocate 6 MiB of
 * staging, 16 MiB of write buffering and 16 KiB of internal SD scratch. Start a
 * priority-4 disk task (16 KiB stack) and priority-3 formatting task (8 KiB
 * stack), both on Core 0. They wait for prepare before processing data.
 * A partial allocation failure requires reset; never allocate a second writer.
 */
bool TimedRecorder::initialize()
{
    bool retVal = _task != nullptr && _formatTask != nullptr;
    if (!retVal && !_staging)
    {
        // Allocate before any source starts. The last argument pins both tasks
        // to Core 0; priorities 4 and 3 are scheduling ranks, not core numbers.
        // Each task sees Phase::idle until prepare publishes a complete request.
        _staging = allocateExternal(_stagingSamples * 3);
        retVal = _staging && _timedBufferedSink.initializeMemory(allocateExternal, releaseMemory) &&
            _timedFatStorage.initializeMemory(allocateInternal, releaseMemory) &&
            xTaskCreatePinnedToCore(taskEntry, "timedWriter", 16384, this, 4, &_task, 0) == pdPASS &&
            xTaskCreatePinnedToCore(formatEntry, "timedFormat", 8192, this, 3, &_formatTask, 0) == pdPASS;
    }
    return retVal;
}

/* **
 * prepare - Control task publishes settings before releasing the preparation
 * request. This verification adapter stops on a full pool: its combined CRC
 * includes all files and cannot verify bytes later overwritten by circular use.
 * The reusable formatter/storage still support circular mode independently.
 */
bool TimedRecorder::prepare(uint32_t run, uint64_t utcFirst, const TimedRecordingConfig& config)
{
    const bool retVal = _task && _formatTask && _phase.load() == Phase::idle && !config._overwrite &&
        config._fileCount && config._fileCount <= 4096 && config._pairsPerFile && config._pairsPerFile <= 65535;
    if (retVal)
    {
        // Save ordinary fields before the atomic state change. The disk task's
        // load of Phase::preparing makes these preceding assignments visible.
        _run = run;
        _utcFirst = utcFirst;
        _config = config;
        _failed.store(false);
        _finishRequested.store(false);
        _phase.store(Phase::preparing);
    }
    return retVal;
}

/* **
 * getReady - Control task may arm acquisition only after all storage is prepared.
 */
bool TimedRecorder::getReady() const
{
    return _phase.load() == Phase::running && !getFailed() && !_finishRequested.load();
}

/* **
 * getFailed - Atomic flags report faults without borrowing disk-task counters.
 */
bool TimedRecorder::getFailed() const
{
    return _failed.load() || _timedIntervalBuffer.getFailed() || _timedBufferedSink.getFailed();
}

/* **
 * submit - Decoder/Core 1 uses the existing three-byte packer, then copies into
 * staging. The local member scratch is used only by this caller. Reject a batch
 * above the decoder's 512-word maximum; do not truncate or retain its pointer.
 */
bool TimedRecorder::submit(const uint32_t* words, size_t count)
{
    bool retVal = !getFailed() && words && count && count <= 512;
    if (retVal)
    {
        const size_t bytes = recordingFormat::packSamples(words, count, _packed, sizeof(_packed));
        retVal = bytes == count * 3 && _timedIntervalBuffer.submit(_packed, uint32_t(count));
    }
    if (!retVal)
    {
        _failed.store(true);
    }
    return retVal;
}

/* **
 * addBoundary - Timing task copies metadata; no formatting or disk work here.
 */
bool TimedRecorder::addBoundary(const TimedBoundary& boundary)
{
    return _timedIntervalBuffer.addBoundary(boundary);
}

/* **
 * finish - Control task calls only after decoding and boundary delivery stop.
 * The atomic request makes clean visible to the formatting task. That task
 * completes its submissions before telling the disk task to close. Return does not mean
 * saving is complete; wait for TIMED SAVED/FAILED before removing the card.
 */
void TimedRecorder::finish(bool clean)
{
    _clean = clean;
    _finishRequested.store(true);
}

/* **
 * taskEntry - FreeRTOS calls with the recorder pointer supplied by initialize.
 */
void TimedRecorder::taskEntry(void* context)
{
    static_cast<TimedRecorder*>(context)->work();
}

/* **
 * work - Disk task owns file calls. Initialize the formatter while it is idle,
 * then publish Phase::running to transfer stream ownership to that task.
 * Preallocate headroom for clock differences; pump one 128-KiB write per cycle.
 * Sleep one scheduler tick after every iteration, matching SdRecorder's passing
 * scheduling pattern. At priority 4 this task otherwise prevents the lower-
 * priority control task from numbering FSYNC for too long when an entire
 * interval becomes ready. taskYIELD alone would not let that lower priority run.
 * A tick is 1 ms in this build; it affects delivery, not hardware timestamps.
 * Final drain waits for the formatter's completion flag; otherwise a late
 * header/sample submission could race file closure or checksum verification.
 */
void TimedRecorder::work()
{
    for (;;)
    {
        if (_phase.load() == Phase::preparing)
        {
            printf("TIMED PREP preparing preserved file pool\n");
            // Reserve a 256-byte header, up to 60 (or configured) pairs, and a
            // 48-byte end marker. 1,050,000 samples per pair provides allocation
            // headroom over nominal 1,024,000; it does not change sampling rate.
            const uint64_t capacity = 256 + uint64_t(_config._pairsPerFile) * (48 + uint64_t(1050000) * 3) + 48;
            const bool ready = _timedFatStorage.prepare(_p4RecordingDisk, _config._fileCount, capacity) &&
                _timedBufferedSink.start(_config._fileCount, timedClock) &&
                _timedRecordingStream.start(_utcFirst, _config) &&
                _timedIntervalBuffer.start(_staging, _stagingSamples, _timedRecordingStream, _config._pairsPerFile);
            if (ready)
            {
                printf("TIMED READY directory=%s utcFirst=%llu syntheticUtc=1\n",
                    _timedFatStorage.getDirectory(), (unsigned long long)_utcFirst);
                // Storage initialized the stream while formatting was idle.
                // From this release onward, only format may modify the stream.
                _phase.store(Phase::running);
            }
            else
            {
                _failed.store(true);
                printf("TIMED FAILED preparation=%s\n", _timedFatStorage.getError());
                _phase.store(Phase::complete);
            }
        }
        if (_phase.load() == Phase::running)
        {
            // Measure elapsed work separately, including preemption by other
            // tasks. These totals are diagnostics, not exclusive CPU usage.
            const int64_t began = esp_timer_get_time();
            _timedBufferedSink.pump();
            _diskUs += uint64_t(esp_timer_get_time() - began);
            ++_workCycles;
            if (_formatFinished.load())
            {
                // This atomic load observes the formatter's final assignments.
                // It is now safe to inspect its counters and close the files.
                complete();
                _phase.store(Phase::complete);
            }
        }
        vTaskDelay(1);
    }
}

/* **
 * formatEntry - FreeRTOS supplies the same recorder object to the formatting task.
 */
void TimedRecorder::formatEntry(void* context)
{
    static_cast<TimedRecorder*>(context)->format();
}

/* **
 * format - After disk preparation, this task alone owns stream calls and staging
 * consumption. Each cycle copies at most 4096 samples, then sleeps one tick so
 * lower-priority control/timing can run. There are no filesystem calls here.
 * Thus an SD write waiting for hardware does not stop filling the 16-MiB ring.
 * After both source submitters stop, drain closed intervals, queue the normal
 * end marker only on success, and release-publish the final statistics/state.
 */
void TimedRecorder::format()
{
    for (;;)
    {
        if (_phase.load() == Phase::running && !_formatFinished.load())
        {
            const int64_t began = esp_timer_get_time();
            const bool progress = !getFailed() && _timedIntervalBuffer.pump();
            _formatUs += uint64_t(esp_timer_get_time() - began);
            if (_finishRequested.load() && !progress)
            {
                // No more source data/boundaries may arrive after this request.
                // finish rejects an unfinished closed interval. It intentionally
                // omits samples after the last supplied closing boundary.
                _formatPassed = _clean && !getFailed() && _timedIntervalBuffer.finish();
                _formatFinished.store(true);
            }
        }
        vTaskDelay(1);
    }
}

/* **
 * beginFile - Formatting callback records file count after its queued opening.
 */
bool TimedRecorder::beginFile(uint32_t iSlot)
{
    const bool retVal = _timedBufferedSink.beginFile(iSlot);
    _files += retVal;
    return retVal;
}

/* **
 * submit - Formatting callback records a checksum of the exact serialized bytes
 * accepted by the write ring, including headers, tBlocks and end markers.
 */
bool TimedRecorder::submit(const uint8_t* bytes, size_t count)
{
    const bool retVal = _timedBufferedSink.submit(bytes, count);
    if (retVal)
    {
        _crc = esp_rom_crc32_le(_crc, bytes, uint32_t(count));
        _bytes += count;
    }
    return retVal;
}

/* **
 * endFile - Queue closure only after the formatter submitted a complete marker.
 */
bool TimedRecorder::endFile()
{
    return _timedBufferedSink.endFile();
}

/* **
 * check - Disk task folds all readback bytes in slot order into a second CRC.
 * This adapter never reuses a slot, so slot order is recording order. Offsets
 * are supplied by TimedFatStorage; it reads only bytes written in this session.
 */
bool TimedRecorder::check(void* context, uint32_t iSlot, uint64_t offset, const uint8_t* bytes, uint32_t count)
{
    (void)iSlot;
    (void)offset;
    auto& recorder = *static_cast<TimedRecorder*>(context);
    recorder._readCrc = esp_rom_crc32_le(recorder._readCrc, bytes, count);
    recorder._readBytes += count;
    return true;
}

/* **
 * complete - Both source submitters and the formatter have stopped. Acquire of
 * formatFinished made formatter totals and its verdict safe to read here.
 * Drain the write ring, close files, then read every retained byte from SD.
 * An acquisition fault omits the normal end marker. Report aggregate counts
 * only after saving/readback; software CRC equality is not GPS time validation.
 */
void TimedRecorder::complete()
{
    bool passed = _formatPassed && !getFailed();
    const bool saved = _timedBufferedSink.finish();
    passed = saved && passed;
    if (passed)
    {
        printf("TIMED VERIFY bytes=%llu\n", (unsigned long long)_bytes);
        passed = _timedFatStorage.readback(check, this) && _readBytes == _bytes && _readCrc == _crc;
    }
    _failed.store(!passed);
    const auto stats = _timedBufferedSink.getStatistics();
    printf("TIMED WORK cycles=%lu formatUs=%llu diskUs=%llu\n",
        (unsigned long)_workCycles, (unsigned long long)_formatUs, (unsigned long long)_diskUs);
    printf("TIMED %s run=%lu directory=%s files=%lu intervals=%llu samples=%llu bytes=%llu crc=%08lx stagePeak=%lu ringPeak=%lu maxWriteUs=%lu errors=%lu unsaved=%lu syntheticUtc=1\n",
        passed ? "SAVED" : "FAILED", (unsigned long)_run, _timedFatStorage.getDirectory(),
        (unsigned long)_files, (unsigned long long)_timedIntervalBuffer.getIntervals(),
        (unsigned long long)_timedIntervalBuffer.getSamples(), (unsigned long long)_bytes,
        (unsigned long)_crc, (unsigned long)_timedIntervalBuffer.getPeak(),
        (unsigned long)stats._peak, (unsigned long)stats._maxUs,
        (unsigned long)stats._errors, (unsigned long)stats._unsavedBytes);
}
