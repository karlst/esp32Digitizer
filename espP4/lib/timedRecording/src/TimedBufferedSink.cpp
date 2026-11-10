// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Connect the formatter's byte stream to the reusable buffered disk writer.
 * The formatting task calls beginFile/submit/endFile: these copy bytes and file
 * boundary descriptors into owned queues without disk I/O. The disk task calls
 * pump/write/closeCompleted/finish to open, write and close the requested files.
 * Read start first, then those two call sequences. The queue's release/acquire
 * indices prevent reuse of descriptors until the disk task has finished them.
 *
 * A file boundary descriptor records logical stream byte offsets, not FSYNC or
 * PPS ticks. The 128-KiB byte-ring chunks can cross file boundaries and tBlocks;
 * write splits them at file ends. TimedFatStorage separately accumulates aligned
 * 16-KiB transfers from each file's byte zero. Neither transfer size defines an
 * sBlock. _written counts bytes accepted by the adapter, including its RAM tail.
 */
#include "TimedBufferedSink.h"
#include <algorithm>
#include <limits>

/* **
 * TimedBufferedSink - Borrow the disk adapter; construction performs no I/O.
 */
TimedBufferedSink::TimedBufferedSink(TimedSlotStorage& timedSlotStorage)
    : _timedSlotStorage(timedSlotStorage)
{
}

/* **
 * initializeMemory - Allocate the existing byte ring before either task starts.
 * The allocator decides internal RAM versus PSRAM; this layer has no board pins.
 */
bool TimedBufferedSink::initializeMemory(uint8_t* (*allocate)(uint32_t), void (*release)(uint8_t*))
{
    const bool retVal = !_active && _bufferedWriter.initializeMemory(allocate, release);
    _memoryReady = _memoryReady || retVal;
    return retVal;
}

/* **
 * start - Reset offsets and boundary ownership while both tasks are stopped.
 * slotCount names an already prepared file pool. clockUs measures elapsed
 * microseconds for write statistics, not sample timestamps or calendar time.
 */
bool TimedBufferedSink::start(uint32_t slotCount, uint64_t (*clockUs)())
{
    const bool retVal = !_active && _memoryReady && slotCount && clockUs;
    if (retVal)
    {
        _slotCount = slotCount;
        _clockUs = clockUs;
        _head.store(0);
        _tail.store(0);
        _submitted = _written = 0;
        _producerOpen = _diskOpen = false;
        _failed.store(false);
        _bufferedWriter.start(*this, clockUs, BufferedWriter::OverflowPolicy::stop);
        _active = true;
    }
    return retVal;
}

/* **
 * beginFile - Reserve a boundary descriptor without opening a physical file.
 * Publish it before any bytes enter the ring. Acquiring tail guarantees the
 * preceding file close finished before a descriptor is reused after wraparound.
 */
bool TimedBufferedSink::beginFile(uint32_t iSlot)
{
    const uint32_t head = _head.load(std::memory_order_relaxed);
    const bool retVal = _active && !getFailed() && !_producerOpen && iSlot < _slotCount &&
        head - _tail.load(std::memory_order_acquire) < _boundaryCount;
    if (retVal)
    {
        auto& boundary = _boundaries[head % _boundaryCount];
        boundary._start = _submitted;
        boundary._iSlot = iSlot;
        boundary._ended.store(false, std::memory_order_relaxed);
        _head.store(head + 1, std::memory_order_release);
        _producerOpen = true;
    }
    if (!retVal)
    {
        _failed.store(true);
    }
    return retVal;
}

/* **
 * submit - Copy bytes into the owned ring and advance the logical stream offset.
 * No disk calls or waits occur here. A refused batch latches failure; callers
 * cannot retry it or quietly drop data. A pointer may be reused after return.
 */
bool TimedBufferedSink::submit(const uint8_t* data, size_t bytes)
{
    bool retVal = _active && !getFailed() && _producerOpen && data && bytes &&
        bytes <= UINT32_MAX && bytes <= std::numeric_limits<uint64_t>::max() - _submitted;
    if (retVal)
    {
        retVal = _bufferedWriter.submit(data, static_cast<uint32_t>(bytes));
    }
    if (retVal)
    {
        _submitted += bytes;
    }
    else
    {
        _failed.store(true);
    }
    return retVal;
}

/* **
 * endFile - Publish the exact end offset after the formatter queued its marker.
 * The disk task must close this file before opening the next boundary. It may
 * already have written a prefix while the end offset was still unknown.
 */
bool TimedBufferedSink::endFile()
{
    bool retVal = _active && !getFailed() && _producerOpen;
    if (retVal)
    {
        auto& boundary = _boundaries[(_head.load(std::memory_order_relaxed) - 1) % _boundaryCount];
        retVal = _submitted > boundary._start;
        if (retVal)
        {
            boundary._end = _submitted;
            boundary._ended.store(true, std::memory_order_release);
            _producerOpen = false;
        }
    }
    if (!retVal)
    {
        _failed.store(true);
    }
    return retVal;
}

/* **
 * closeCompleted - Disk task closes a file after all its bytes were accepted by
 * the storage adapter and the formatter supplied its exact end offset. The
 * adapter may still hold a short RAM tail; closeSlot writes and flushes it.
 * The formatter may publish end after the final bytes were pumped, so this is also
 * called from pump when no additional byte chunk is available. Tail is released
 * only after close succeeds. On failure the descriptor remains unavailable.
 */
bool TimedBufferedSink::closeCompleted()
{
    bool retVal = true;
    const uint32_t tail = _tail.load(std::memory_order_relaxed);
    if (_diskOpen && tail != _head.load(std::memory_order_acquire))
    {
        const auto& boundary = _boundaries[tail % _boundaryCount];
        if (boundary._ended.load(std::memory_order_acquire) && _written == boundary._end)
        {
            const uint64_t began = _clockUs();
            retVal = _timedSlotStorage.closeSlot();
            // Closing can flush to the card even outside a byte-write call.
            // Include that time and any failure in the reusable statistics.
            _bufferedWriter.measure(began, 2, retVal);
            _diskOpen = false;
            if (retVal)
            {
                _tail.store(tail + 1, std::memory_order_release);
            }
            else
            {
                _failed.store(true);
            }
        }
    }
    return retVal;
}

/* **
 * write - BufferedWriter calls on the disk task with one owned ring span.
 * Split only at file boundaries; the storage adapter collects bytes into its
 * DMA-safe transfer buffer. For example, a chunk containing the final 100 bytes
 * of one file and 500 bytes of the next produces two adapter submissions with
 * a file close/open between them, not necessarily two immediate physical writes.
 * Never release or retry a partially written span; BufferedWriter accounts it.
 */
size_t TimedBufferedSink::write(const uint8_t* data, size_t bytes)
{
    size_t retVal = 0;
    while (retVal < bytes && !_failed.load())
    {
        const bool closed = closeCompleted();
        const uint32_t tail = _tail.load(std::memory_order_relaxed);
        if (!closed || tail == _head.load(std::memory_order_acquire))
        {
            _failed.store(true);
        }
        else
        {
            const auto& boundary = _boundaries[tail % _boundaryCount];
            if (!_diskOpen)
            {
                _diskOpen = _written == boundary._start && _timedSlotStorage.openSlot(boundary._iSlot);
                if (!_diskOpen)
                {
                    _failed.store(true);
                }
            }
            if (_diskOpen && !_failed.load())
            {
                size_t amount = bytes - retVal;
                if (boundary._ended.load(std::memory_order_acquire))
                {
                    amount = static_cast<size_t>(std::min<uint64_t>(amount, boundary._end - _written));
                }
                const size_t accepted = _timedSlotStorage.write(data + retVal, amount);
                if (!amount || accepted > amount)
                {
                    _failed.store(true);
                }
                else
                {
                    retVal += accepted;
                    _written += accepted;
                    if (accepted != amount || _timedSlotStorage.getFailed())
                    {
                        _failed.store(true);
                    }
                }
            }
        }
    }
    return retVal;
}

/* **
 * pump - Disk task makes at most one ordinary ring-chunk write per call.
 * Keep drain=false during acquisition; true permits the final short chunk.
 * No formatter or sample decoding runs inside this call.
 */
bool TimedBufferedSink::pump(bool drain)
{
    const bool written = _bufferedWriter.pump(drain);
    const bool closed = closeCompleted();
    return written && closed && !getFailed();
}

/* **
 * flush - BufferedWriter calls after all submissions stop. Close a complete
 * final file, or close an incomplete failed file without calling it successful.
 * Never create a completion marker here; only the formatter may submit one.
 */
bool TimedBufferedSink::flush()
{
    const bool completed = closeCompleted();
    const bool retVal = completed && !_producerOpen && !_diskOpen &&
        _tail.load() == _head.load() && !_failed.load();
    if (_diskOpen)
    {
        _timedSlotStorage.flush();
        _timedSlotStorage.closeSlot();
        _diskOpen = false;
    }
    if (!retVal)
    {
        _failed.store(true);
    }
    return retVal;
}

/* **
 * finish - Disk task drains queued bytes and closes files after producer stop.
 * Caller must first join/stop the submitting task; this is not a concurrent stop
 * request. Failures propagate even when an accepted prefix reached the card.
 */
bool TimedBufferedSink::finish()
{
    const bool retVal = _bufferedWriter.finish() && !getFailed();
    _active = false;
    return retVal;
}

/* **
 * getFailed - Either task can query the latched queue or byte-writer fault.
 */
bool TimedBufferedSink::getFailed() const
{
    return _failed.load() || _bufferedWriter.getFailed();
}

/* **
 * getStatistics - Disk task obtains byte counts and write/flush durations.
 * Other tasks may read the returned copy only after finish or synchronization.
 */
BufferedWriter::Statistics TimedBufferedSink::getStatistics()
{
    return _bufferedWriter.getStatistics();
}
