// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Reading path: initialize creates the tasks; prepare requests a new file pool;
 * work prepares storage and enables format. Decoder submit copies packed samples
 * into staging, while addBoundary copies timing records. format consumes both
 * queues and fills the write ring; work drains that ring to the card. finish
 * requests shutdown; complete verifies the card only after format has stopped.
 * These are separate tasks (scheduled software threads), not interrupt handlers.
 *
 * Ownership on the P4:
 * - Core 1 decoder: packSamples removes status bits; TimedIntervalBuffer::submit
 *   copies three-byte samples into 6-MiB staging without waiting for disk I/O.
 * - Core 0 control/timing: addBoundary supplies PPS/FSYNC ticks and sample index.
 * - Core 0 formatter (priority 3): waits for a complete interval, serializes its
 *   tBlock and samples, and copies them into the separate 16-MiB byte ring.
 * - Core 0 disk task (priority 4): drains up to one 128-KiB chunk per work pass.
 *   TimedFatStorage turns those bytes into file-aligned 16-KiB SD transfers.
 * The timed test gives control priority 5 so it can preempt storage tasks for
 * millisecond FSYNC observations. Interrupts and critical sections can still
 * delay it; priority is not a timing guarantee. Sleeps allow lower-priority tasks to
 * run; captured timestamps come from hardware, not the scheduling time.
 *
 * finish requests completion; TIMED SAVED confirms drain, file close and full
 * CRC (Cyclic Redundancy Check) readback. This bench adapter reports synthetic
 * UTC and does not establish GPS lock or an absolute timing-accuracy bound.
 */
#include "TimedRecorder.h"
#include "recordingFormat.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"
#include <cstdio>

/* **
 * allocateExternal - Initialization callback reserves bytes in external PSRAM
 * (the board's additional RAM), for staging or the write ring. Returns null on
 * allocation failure. The SD driver does not directly transfer from this memory.
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
        // File-operation diagnostics use the same monotonic clock as the writer.
        // The disk task collects them without console output during acquisition.
        _timedFatStorage.setTimingClock(timedClock);

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
 * addBoundary - Core 0 timing supplies one saved PPS and adjacent FSYNC pair.
 * TimedIntervalBuffer::addBoundary copies the record into its queue for the
 * formatter. False latches a boundary/queue fault there; no caller pointer is
 * retained. No formatting or disk work occurs on this timing-task call.
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
 * scheduling pattern. Sleeping allows lower-priority work to run; taskYIELD
 * alone would not do so. The timed bench now raises control to priority 5,
 * above this writer, but preserves the sleep and bounded write work.
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
                // Start the disk-task trace after preallocation, before capture.
                _backlogCount = 0;
                _backlogStartedUs = uint64_t(esp_timer_get_time());
                _backlogLastUs = 0;
                recordBacklog(true);
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
            recordBacklog();
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
 * other work can run. The timed bench's priority-5 control task can also preempt
 * this formatter. There are no filesystem calls here.
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
 * beginFile - TimedRecordingStream calls on the formatting task with a zero-based
 * slot index. Queue its opening and count accepted file requests. True means
 * queued, not opened on SD; a failed request remains a recording fault.
 */
bool TimedRecorder::beginFile(uint32_t iSlot)
{
    const bool retVal = _timedBufferedSink.beginFile(iSlot);
    _files += retVal;
    return retVal;
}

/* **
 * submit - TimedRecordingStream calls on the formatting task with count bytes,
 * unlike the decoder-facing submit overload whose count is samples. The byte
 * ring copies them before return, so the caller can reuse its buffer. On true,
 * update CRC and byte total for the exact accepted header/tBlock/sample/marker
 * stream. This baseline is later compared with SD readback, not just RAM copies.
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
 * After each 256 KiB of readback, sleep one scheduler tick so the idle task can
 * service its watchdog. This runs after acquisition; it cannot delay samples.
 */
bool TimedRecorder::check(void* context, uint32_t iSlot, uint64_t offset, const uint8_t* bytes, uint32_t count)
{
    (void)iSlot;
    auto& recorder = *static_cast<TimedRecorder*>(context);
    recorder._readCrc = esp_rom_crc32_le(recorder._readCrc, bytes, count);
    recorder._readBytes += count;
    if ((offset + count) / 262144 != offset / 262144)
    {
        vTaskDelay(1);
    }
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
    // Preserve the backlog before finish drains or abandons queued bytes.
    recordBacklog(true);
    bool passed = _formatPassed && !getFailed();
    const bool saved = _timedBufferedSink.finish();
    passed = saved && passed;

    // All writing has stopped. Report each file separately so slow ordinary
    // writes can be distinguished from closing/opening at a rotation. These
    // elapsed durations include preemption, so they do not prove card latency.
    const char* operationNames[] = {"open", "copy", "write", "sync", "close"};
    const auto& timings = _timedFatStorage.getTimings();
    for (size_t iSlot = 0; iSlot < timings.size(); ++iSlot)
    {
        for (unsigned iOperation = 0; iOperation < unsigned(TimedFatStorage::Operation::count); ++iOperation)
        {
            const auto& timing = timings[iSlot]._operations[iOperation];
            if (timing._calls != 0)
            {
                printf("TIMED IO slot=%lu operation=%s calls=%llu totalUs=%llu maxUs=%llu atUs=%llu offset=%llu requested=%lu actual=%lu result=%lu failures=%lu\n",
                    (unsigned long)iSlot, operationNames[iOperation],
                    (unsigned long long)timing._calls, (unsigned long long)timing._totalUs,
                    (unsigned long long)timing._maxUs, (unsigned long long)timing._atUs,
                    (unsigned long long)timing._offset, (unsigned long)timing._requested,
                    (unsigned long)timing._actual, (unsigned long)timing._result,
                    (unsigned long)timing._failures);
                // Reporting is post-acquisition; let lower-priority tasks run.
                vTaskDelay(1);
            }
        }
    }
    if (passed)
    {
        printf("TIMED VERIFY bytes=%llu\n", (unsigned long long)_bytes);
        passed = _timedFatStorage.readback(check, this) && _readBytes == _bytes && _readCrc == _crc;
    }
    _failed.store(!passed);
    const auto stats = _timedBufferedSink.getStatistics();
    for (unsigned iSample = 0; iSample < _backlogCount; ++iSample)
    {
        const auto& sample = _backlogSamples[iSample];
        printf("TIMED BACKLOG elapsedUs=%llu writtenBytes=%llu queuedBytes=%lu peakBytes=%lu overflows=%lu pendingBytes=%lu\n",
            (unsigned long long)sample._elapsedUs, (unsigned long long)sample._writtenBytes,
            (unsigned long)sample._queuedBytes, (unsigned long)sample._peakBytes,
            (unsigned long)sample._overflows, (unsigned long)sample._pendingBytes);
        // Up to 320 rows: do not monopolize Core 0 while printing the trace.
        vTaskDelay(1);
    }
    printf("TIMED BACKLOG LIMIT: disk-task snapshots after writes; blocked writes delay rows; writtenBytes excludes pending transfer bytes; queuedBytes is ring only; final row precedes drain\n");
    printf("TIMED WORK cycles=%lu formatUs=%llu diskUs=%llu\n",
        (unsigned long)_workCycles, (unsigned long long)_formatUs, (unsigned long long)_diskUs);
    printf("TIMED %s run=%lu directory=%s files=%lu intervals=%llu samples=%llu bytes=%llu crc=%08lx stagePeak=%lu ringPeak=%lu maxWriteUs=%lu errors=%lu unsaved=%lu syntheticUtc=1\n",
        passed ? "SAVED" : "FAILED", (unsigned long)_run, _timedFatStorage.getDirectory(),
        (unsigned long)_files, (unsigned long long)_timedIntervalBuffer.getIntervals(),
        (unsigned long long)_timedIntervalBuffer.getSamples(), (unsigned long long)_bytes,
        (unsigned long)_crc, (unsigned long)_timedIntervalBuffer.getPeak(),
        (unsigned long)stats._peak, (unsigned long)stats._maxUs,
        (unsigned long)stats._errors, (unsigned long)(stats._unsavedBytes + _timedFatStorage.getPendingBytes()));
}

/* **
 * recordBacklog - Disk task samples the existing writer statistics about once
 * per second. final also takes an immediate snapshot before shutdown drain.
 * No other task reads the writer-owned byte total. Queue occupancy is the
 * writer's atomic snapshot of bytes waiting in its ring, not ADC staging.
 * Differences in writtenBytes divided by differences in elapsedUs give the
 * observed write rate. A blocked write delays this method too; retain actual
 * elapsed times rather than inventing measurements for the missing seconds.
 * Fixed storage covers this five-minute test. Rows are printed only at complete.
 * writtenBytes means bytes accepted by FatFs after subtracting the adapter's
 * pending tail. It is not readback verification or a power-loss durability claim.
 * queuedBytes/peakBytes describe the byte ring only, not interval staging.
 */
void TimedRecorder::recordBacklog(bool final)
{
    const uint64_t now = uint64_t(esp_timer_get_time());
    if (_backlogCount < 320 && (final || now - _backlogLastUs >= 1000000))
    {
        const auto stats = _timedBufferedSink.getStatistics();
        // The ring counts bytes copied into the adapter as accepted. Subtract
        // the adapter's retained tail to report only bytes accepted by FatFs.
        const uint32_t pending = _timedFatStorage.getPendingBytes();
        _backlogSamples[_backlogCount++] = {now - _backlogStartedUs, stats._bytes - pending,
            stats._used, stats._peak, stats._overflows, pending};
        _backlogLastUs = now;
    }
}
