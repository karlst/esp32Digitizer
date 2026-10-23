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
 * @file BufferedWriter.cpp
 * Copy quickly into RAM, then write whole blocks from the storage task.
 * Read start(), submit(), pump(), finish() in that order. No sample or board
 * knowledge belongs here; byteSink supplies storage and the caller supplies time.
 *
 * A ring buffer is a fixed array reused in a circle. It temporarily holds data
 * while the SD card is busy. The producer ADDS bytes; the consumer REMOVES them
 * only after writing them. The current caller is P4:
 * Core 1 SdRecorder::submitSamples() -> FileRecordingBackend::copyToRingBuffer() -> submit();
 * Core 0 SdRecorder::work()
 *     -> FileRecordingBackend::pump() -> pump() -> FileRecordingBackend::write()
 *     -> card. P4 supplies three-byte samples and uses a 16 MiB PSRAM ring
 *     with 128 KiB write batches. The library itself
 *     treats these as bytes; it does not understand ADC frames or status bits.
 *
 * head is the total producer position, tail the total consumer position. They
 * keep advancing even when the physical array position wraps back to zero.
 * head==tail means empty; head-tail==capacity means full. Keeping the running
 * counts distinguishes those cases even though their array indexes can match.
 * Unsigned subtraction remains valid when the 32-bit counters wrap, provided
 * their distance never exceeds the ring capacity (submit enforces that limit).
 */
#include "BufferedWriter.h"
#include <algorithm>
#include <cstring>

// C++11 needs storage definitions when std::min binds these constants by reference.
constexpr uint32_t BufferedWriter::_capacity;
constexpr uint32_t BufferedWriter::_blockBytes;

#if BUFFERED_WRITER_EXTERNAL

/* **
 * initializeMemory - Allocate the ring buffer before any task uses it. allocate
 * takes a byte count and returns the memory address. Keep release so the
 * destructor can later free that same memory.
 *
 * Return true if the buffer was already allocated or allocation succeeds now.
 * Repeated calls reuse the allocation. This prepares RAM only; it does not start
 * recording or open a file.
 */
bool BufferedWriter::initializeMemory(uint8_t* (*allocate)(uint32_t), void (*release)(uint8_t*))
{
    bool retVal = _buffer != nullptr;
    if (!_active && !_buffer && allocate && release)
    {
        _buffer = allocate(_capacity);
        if (_buffer)
        {
            _releaseMemory = release;
            retVal = true;
        }
    }
    return retVal;
}

/* **
 * ~BufferedWriter - Free the ring memory using the release function saved during
 * initialization. All tasks must have stopped using the buffer. The destructor
 * does not write pending data; finish must be called first.
 */
BufferedWriter::~BufferedWriter()
{
    if (_buffer && _releaseMemory)
    {
        _releaseMemory(_buffer);
    }
}
#endif

/* **
 * start - Reset the ring for a new recording. Both the task adding bytes and the
 * task writing them must be stopped while this runs. Resetting the read and
 * write positions makes the buffer empty without clearing its memory.
 *
 * byteSink supplies the actual write and flush functions; clock returns elapsed
 * microseconds. Keep both available until recording finishes. overflowPolicy
 * chooses whether a full ring stops further submissions or only rejects the
 * current batch. This call does not open an SD file or start the ADC.
 */
void BufferedWriter::start(ByteSink& byteSink, uint64_t (*clock)(), OverflowPolicy overflowPolicy)
{
    // Keep the storage adapter and clock for later calls; allocate no per-sample memory.
    _byteSink = &byteSink;
    _clockUs = clock;
    _overflowPolicy = overflowPolicy;

    // Both counters restart together. Neither producer nor consumer may be using
    // the previous recording when this reset happens.
    _head.store(0);
    _tail.store(0);
    _peak.store(0);
    _rejected.store(0);
#if BUFFERED_WRITER_EXTERNAL
    _broken.store(_buffer == nullptr);
#else
    _broken.store(false);
#endif
    _overflowed.store(false);
    _partialBytes = 0;
    _statistics = {};
    _startedUs = _clockUs();
    _active = true;
}

/* **
 * submit - Copy the entire batch into the ring, or reject it without waiting.
 * data points to the bytes to copy and length is their byte count. Return true
 * when the bytes are in RAM, not when they are on SD. The caller can reuse its
 * input memory after this returns.
 *
 * On P4, only the Core 1 decoder submits data. The disk task may simultaneously
 * write older bytes from another part of the ring. start must have run first,
 * and submission must stop before finish or cancel.
 *
 * If the batch will not fit, count all its bytes as rejected. The stop policy
 * also prevents later submissions until start resets the writer. dropIncoming
 * allows later batches once space becomes available, but saved data on either
 * side of a loss will be adjacent; the caller must account for missing samples.
 */
bool BufferedWriter::submit(const uint8_t* data, uint32_t length)
{
    // head belongs to this producer. relaxed is sufficient for our own position.
    // The other core owns tail; acquire means observe space only after that core
    // has finished using it. These are atomic memory operations, not mutex locks.
    const uint32_t writeAt = _head.load(std::memory_order_relaxed);
    const uint32_t occupied = writeAt - _tail.load(std::memory_order_acquire);

    // head - tail is bytes occupied; capacity - occupied is bytes still free.
    // Seeing an older tail can make us conservative about free space, never unsafe.
    const bool retVal = !_broken.load(std::memory_order_relaxed) && length <= _capacity - occupied;
    if (retVal)
    {
#if BUFFERED_WRITER_EXTERNAL

        // Task-only PSRAM path: copy the span up to the array end, then any
        // remainder from index zero. For example, 12 bytes starting 4 bytes
        // before the end need two copies of 4 and 8 bytes. Update _head after both.
        const uint32_t offset = writeAt & (_capacity - 1);
        const uint32_t first = std::min(length, _capacity - offset);
        std::memcpy(_buffer + offset, data, first);
        if (length > first)
        {
            std::memcpy(_buffer, data + first, length - first);
        }
#else

        // Internal-RAM ISR path avoids calling a flash-resident memcpy.
        for (uint32_t iByte = 0; iByte < length; ++iByte)
        {
            _buffer[(writeAt + iByte) & (_capacity - 1)] = data[iByte];
        }
#endif
        if (occupied + length > _peak.load(std::memory_order_relaxed))
        {
            _peak.store(occupied + length, std::memory_order_relaxed);
        }

        // Only now advertise the new bytes. release ensures their contents become
        // visible to the consumer BEFORE it sees the advanced head position.
        _head.store(writeAt + length, std::memory_order_release);
    }
    else
    {
        if (length > _capacity - occupied)
        {
            _overflowed.store(true, std::memory_order_relaxed);
        }

        // Only this producer changes rejected, so no atomic read/modify/write
        // instruction or interrupt-unsafe library helper is needed.
        _rejected.store(_rejected.load(std::memory_order_relaxed) + length, std::memory_order_relaxed);
        if (_overflowPolicy == OverflowPolicy::stop)
        {
            _broken.store(true, std::memory_order_release);
        }
    }
    return retVal;
}

/* **
 * measure - Record how long a disk operation took. The disk task supplies
 * beganUs, the clock value before the operation, and success, its result. kind
 * identifies a write (1) or flush (2).
 *
 * Save the latest duration, longest duration and when that longest operation
 * ended. Times are microseconds since this recording began. A failed operation
 * increments the error count and sets the shared failure flag. This function
 * records results; it does not retry the operation.
 */
void BufferedWriter::measure(uint64_t beganUs, uint32_t kind, bool success)
{
    const uint64_t now = _clockUs();

    // This is wall-clock duration, including time waiting for the card. Clamp
    // the 32-bit display value rather than letting a very long delay wrap to zero.
    const uint64_t duration = now - beganUs;
    _statistics._latestUs = static_cast<uint32_t>(std::min<uint64_t>(duration, UINT32_MAX));
    _statistics._elapsedUs = now - _startedUs;
    if (_statistics._latestUs > _statistics._maxUs)
    {
        _statistics._maxUs = _statistics._latestUs;
        _statistics._maxAtUs = now - _startedUs;
        _statistics._maxKind = kind;
    }
    if (!success)
    {
        ++_statistics._errors;
        _broken.store(true, std::memory_order_release);
    }
}

/* **
 * pump - Write one batch of the oldest queued bytes. Only the disk task calls
 * this. drain is false during recording so small submissions can accumulate into
 * an efficient batch; true allows a short final batch after submissions stop.
 *
 * The call _byteSink->write(_buffer + offset, length) gives the destination a
 * pointer directly into the ring. On P4 that calls FileRecordingBackend::write.
 * The bytes remain occupied until the entire write succeeds, so the task adding
 * samples cannot overwrite them while SD is busy.
 *
 * If the batch reaches the end of the ring array, a later pump writes the
 * remainder from index zero. A short write records an error and is not retried,
 * because part of it may already be on disk. Return false after a write error.
 * True can mean no write was needed yet, not necessarily that any bytes were
 * written.
 */
bool BufferedWriter::pump(bool drain)
{
    bool retVal = _statistics._errors == 0;

    // tail identifies the oldest queued byte. We own tail; head is updated by
    // the producer. acquire ensures we see the copied data behind its new head.
    const uint32_t readAt = _tail.load(std::memory_order_relaxed);
    const uint32_t occupied = _head.load(std::memory_order_acquire) - readAt;
    if (_active && retVal && occupied && (drain || occupied >= _blockBytes))
    {
        // Convert the running byte count to an array index. For example, twelve
        // bytes starting four bytes before capacity wrap eight bytes to zero.
        const uint32_t offset = readAt & (_capacity - 1);

        // Take the smallest of queued bytes, our configured batch limit, and bytes
        // remaining before the array ends. A write cannot read across the array end;
        // the next pump call handles the wrapped portion at index zero.
        const uint32_t length = std::min<uint32_t>(std::min<uint32_t>(occupied, _blockBytes), _capacity - offset);

        // Start timing immediately before the storage call, including its wait.
        const uint64_t began = _clockUs();

        // This call reads the ring memory and writes the file. _byteSink is the ByteSink
        // interface; the P4 implementation is FileRecordingBackend::write(). It must
        // finish reading before returning.
        // It can block on storage. Crucially, tail has NOT moved yet.
        const size_t written = _byteSink->write(_buffer + offset, length);

        // Count only the byte count the destination reported accepting. A short
        // write can leave part of a sample on disk; never call that a complete block.
        _statistics._bytes += std::min<size_t>(written, length);
        retVal = written == length;
        if (!retVal)
        {
            // Keep track of the prefix already accepted, but leave this whole block
            // occupied. Retrying it would write that prefix twice and corrupt ordering.
            _partialBytes = static_cast<uint32_t>(std::min<size_t>(written, length));
        }
        measure(began, 1, retVal);
        if (retVal)
        {
            // The entire block was accepted. Release its memory for reuse; acquire
            // in submit() pairs with this store. No shifting/copying of remaining data
            // is necessary: moving the position is enough to remove bytes from the queue.
            _tail.store(readAt + length, std::memory_order_release);
        }
    }
    return retVal;
}

/* **
 * finish - Write all remaining queued bytes and flush the destination. Call from
 * the disk task only after the task submitting data has stopped. pump(true)
 * handles the final short batch; flush asks the destination to finish pending
 * writes. Closing the file is the adapter's responsibility.
 *
 * After a write error, try to flush what reached the destination but never retry
 * the partially written batch. Return false if the writer has failed. With
 * dropIncoming, success still does not prove zero loss; the caller must check
 * the rejected-byte count. Statistics remain available after finish.
 */
bool BufferedWriter::finish()
{
    while (_tail.load() != _head.load() && _statistics._errors == 0)
    {
        pump(true);
    }

    // After draining (or a failed write), try to synchronize what did reach the
    // destination. A flush cannot recover bytes rejected by a full ring.
    const uint64_t began = _clockUs();
    const bool flushed = _byteSink->flush();
    measure(began, 2, flushed);
    _active = false;
    const bool retVal = !getFailed();
    return retVal;
}

/* **
 * cancel - End an attempt before any data has been submitted, without flushing
 * the destination. This records its elapsed time and marks the writer inactive.
 * It does not clear errors or discard queued bytes; a recording with submitted
 * data must use finish instead.
 */
void BufferedWriter::cancel()
{
    _statistics._elapsedUs = _clockUs() - _startedUs;
    _active = false;
}

/* **
 * getFailed - Return the failure flag. Either the sample-submission task or disk
 * task may read it. A disk error sets it; a full ring also sets it under the
 * stop policy. dropIncoming instead keeps overflow in the statistics. start
 * clears it for a new recording.
 */
bool BufferedWriter::getFailed() const
{
    const bool retVal = _broken.load(std::memory_order_acquire);
    return retVal;
}

/* **
 * getStatistics - Copy the current write counters and ring-buffer usage. Only
 * the disk task may call this because it owns the non-atomic statistics. Another
 * task must receive a safely shared copy.
 *
 * The sample-submission task can add bytes during this copy, so usage and peak
 * values may represent slightly different moments. Occupied bytes include a
 * write still in progress. Unsaved bytes exclude the part of a failed write that
 * the destination already accepted. After finish or cancel, elapsed time stops
 * increasing.
 */
BufferedWriter::Statistics BufferedWriter::getStatistics()
{
    Statistics retVal = _statistics;
    const uint32_t readAt = _tail.load(std::memory_order_relaxed);
    const uint32_t writeAt = _head.load(std::memory_order_acquire);
    retVal._used = writeAt - readAt;
    retVal._peak = std::max(retVal._used, _peak.load(std::memory_order_relaxed));
    retVal._readPosition = readAt & (_capacity - 1);
    retVal._writePosition = writeAt & (_capacity - 1);
    retVal._rejectedBytes = _rejected.load(std::memory_order_relaxed);
    retVal._overflows = _overflowed.load(std::memory_order_relaxed) ? 1 : 0;

    // A partial-write prefix is still in occupied memory but was already counted
    // as accepted by the file. Subtract it when reporting unsaved queued bytes.
    retVal._unsavedBytes = retVal._used - _partialBytes;

    // After finish/cancel, use the frozen elapsed time. Leaving the page open
    // after stopping must not make the completed recording appear to grow longer.
    if (_active)
    {
        retVal._elapsedUs = _clockUs() - _startedUs;
    }
    return retVal;
}

/* **
 * getQueuedBytes - Return the current number of occupied ring bytes without
 * reading the disk. Call from either the single task submitting data or the
 * single disk-writing task, not while start resets the counters. This observes
 * available space; it does not reserve space for a later submission.
 */
uint32_t BufferedWriter::getQueuedBytes() const
{
    const uint32_t written = _head.load(std::memory_order_acquire);
    const uint32_t retVal = written - _tail.load(std::memory_order_acquire);
    return retVal;
}
