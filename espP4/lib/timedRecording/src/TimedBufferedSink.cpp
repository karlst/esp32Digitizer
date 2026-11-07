// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// Reading path: start, producer begin/submit/end, consumer pump/write/close, finish.
// Disk calls exist only below write/closeCompleted/flush, never in submission.
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
 * closeCompleted - Disk task closes a fully written file whose end was published.
 * A producer may publish end after the final bytes were pumped, so this is also
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
 * Split only at file boundaries; the storage adapter may further split a disk
 * transfer to its DMA scratch size. For example, a chunk containing the final
 * 100 bytes of one file and 500 bytes of the next produces two file writes.
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
