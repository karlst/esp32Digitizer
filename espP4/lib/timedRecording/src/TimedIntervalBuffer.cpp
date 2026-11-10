// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Turn decoded samples and saved PPS boundaries into tBlock/sBlock pairs.
 * Read start, submit/addBoundary, pump, finish in that order. On the P4, Core 1's
 * decoder calls submit; Core 0's timing task calls addBoundary; a separate Core 0
 * formatting task calls pump. A task is a scheduled software thread. No hardware
 * register reads or disk writes occur here.
 *
 * Samples are packed 24-bit readings (three bytes each). A boundary contains
 * PPS ticks plus two consecutive FSYNC edges selected shortly after PPS. The
 * later sample starts an interval; the earlier sample at the NEXT boundary ends
 * it. pump waits for two boundaries AND all samples, writes the tBlock first,
 * then copies the sBlock through TimedRecordingStream into the separate SD ring.
 */
#include "TimedIntervalBuffer.h"
#include <algorithm>
#include <cstring>

/* **
 * start - Reset only while all three callers are stopped. memory holds capacity
 * three-byte samples, numbered from zero; capacity is samples, not bytes, and
 * must be a power of two for masked ring positions. The already-started stream must live
 * through finish. False means invalid configuration; no acquisition may start.
 */
bool TimedIntervalBuffer::start(uint8_t* memory, uint32_t capacity,
    TimedRecordingStream& stream, uint32_t pairsPerFile)
{
    const bool retVal = memory && capacity && !(capacity & (capacity - 1)) &&
        capacity <= 0x10000000 && pairsPerFile && pairsPerFile <= 65535;
    _memory = memory;
    _capacity = capacity;
    _pairsPerFile = pairsPerFile;
    _stream = &stream;
    _head.store(0);
    _tail.store(0);
    _peak.store(0);
    _boundaryHead.store(0);
    _boundaryTail.store(0);
    _sampleRead = _intervals = _samples = 0;
    _remaining = 0;
    _failed.store(!retVal);
    return retVal;
}

/* **
 * submit - Decoder copies a complete packed batch without waiting or disk I/O.
 * head/tail count samples, not bytes. At the array end, split memcpy into two
 * pieces; multiplying the masked sample position by three locates its bytes.
 * Publish head only after both pieces are copied. Failure accepts no samples.
 */
bool TimedIntervalBuffer::submit(const uint8_t* packed, uint32_t count)
{
    const uint32_t head = _head.load(std::memory_order_relaxed);
    const uint32_t used = head - _tail.load(std::memory_order_acquire);
    const bool retVal = !getFailed() && packed && count && used <= _capacity && count <= _capacity - used;
    if (retVal)
    {
        // For example, with 256 slots, a batch of 10 starting at slot 250 copies
        // six samples to the end and four to slot zero. Tail has not advanced
        // while its data are being used by the formatter, so neither copy can
        // overwrite a sample still in use. Head becomes visible only afterward.
        const uint32_t position = head & (_capacity - 1);
        const uint32_t first = std::min(count, _capacity - position);
        std::memcpy(_memory + size_t(position) * 3, packed, size_t(first) * 3);
        std::memcpy(_memory, packed + size_t(first) * 3, size_t(count - first) * 3);
        _peak.store(std::max(_peak.load(std::memory_order_relaxed), used + count));
        _head.store(head + count, std::memory_order_release);
    }
    else
    {
        _failed.store(true);
    }
    return retVal;
}

/* **
 * addBoundary - Timing task copies one TimedBoundary into the eight-entry queue.
 * It supplies the PPS tick count, both adjacent FSYNC tick counts, and the
 * absolute sample index of the later FSYNC. This is not a tBlock yet: the next
 * boundary supplies this interval's closing PPS and last-sample tick count.
 * The formatting task retains its opening entry until the whole interval has
 * been copied. No shared 64-bit value is read before head's release/acquire.
 * Reject invalid spacing or queue overflow; never infer a missing boundary.
 */
bool TimedIntervalBuffer::addBoundary(const TimedBoundary& boundary)
{
    const uint64_t spacing = boundary._tcFsAfter - boundary._tcFsBefore;
    const uint32_t head = _boundaryHead.load(std::memory_order_relaxed);
    const bool retVal = !getFailed() && head - _boundaryTail.load(std::memory_order_acquire) < _boundaryCapacity &&
        boundary._sampleFirst > 0 && boundary._tcPps <= boundary._tcFsBefore &&
        boundary._tcFsAfter > boundary._tcFsBefore && (spacing == 156 || spacing == 157) &&
        boundary._tcFsAfter - boundary._tcPps <= 1600000;
    if (retVal)
    {
        _boundaries[head % _boundaryCapacity] = boundary;
        _boundaryHead.store(head + 1, std::memory_order_release);
    }
    else
    {
        _failed.store(true);
    }
    return retVal;
}

/* **
 * release - Formatting task relinquishes count samples only after a successful copy
 * or an intentional pre-first-boundary discard. sampleRead is extended to 64
 * bits; head/tail differences use unsigned wraparound, while masking with
 * capacity-1 converts the counters to physical array positions. Updating tail
 * with release ordering lets the decoder reuse only these completed slots.
 */
void TimedIntervalBuffer::release(uint32_t count)
{
    _sampleRead += count;
    _tail.store(_tail.load(std::memory_order_relaxed) + count, std::memory_order_release);
}

/* **
 * pump - Formatting task does at most one 4096-sample copy. True means progress;
 * false means waiting or failure, distinguished by getFailed. Once the closing
 * boundary arrives, emit its tBlock before any sBlock bytes. The opening pair's
 * later sample starts this block; the closing pair's earlier sample ends it.
 * No sample is omitted between complete intervals. Only startup samples before
 * the first boundary and shutdown samples after the last boundary are omitted.
 */
bool TimedIntervalBuffer::pump()
{
    bool retVal = false;
    const uint32_t iBoundary = _boundaryTail.load(std::memory_order_relaxed);
    const uint32_t boundaries = _boundaryHead.load(std::memory_order_acquire) - iBoundary;
    uint32_t available = _head.load(std::memory_order_acquire) - _tail.load(std::memory_order_relaxed);
    if (!getFailed() && boundaries)
    {
        const auto& first = _boundaries[iBoundary % _boundaryCapacity];

        // Acquisition began before the first retained PPS interval. Discard
        // only that startup prefix, possibly over several incoming batches.
        if (_sampleRead < first._sampleFirst)
        {
            const uint32_t skip = uint32_t(std::min<uint64_t>(available, first._sampleFirst - _sampleRead));
            release(skip);
            available -= skip;
            retVal = skip != 0;
        }
        if (!_remaining && boundaries >= 2 && _sampleRead == first._sampleFirst)
        {
            // Subtract sample indices, not tick counts, to determine sCount.
            // Example: opening sampleFirst=110 and closing sampleFirst=213
            // retain samples 110..212, count=103. The closing boundary's earlier
            // FSYNC belongs to sample 212; its later FSYNC starts the next block.
            const auto& last = _boundaries[(iBoundary + 1) % _boundaryCapacity];
            const uint64_t count = last._sampleFirst - first._sampleFirst;
            const uint64_t ticks = last._tcPps - first._tcPps;
            if (last._sampleFirst <= first._sampleFirst || count < 2 || count > _capacity ||
                ticks < 158400000 || ticks > 161600000)
            {
                _failed.store(true);
            }
            else if (available >= count)
            {
                // Wait for every sample in this closed interval before emitting
                // its tBlock. DMA delivery can lag the hardware boundary capture.
                TimeBlock block;
                // Header names in the client document are tcPpsCurrent/Next;
                // the existing C++ fields tcPpsFirst/Last store those same two
                // consecutive PPS counts. nSec restarts at zero each file.
                block._tcFsFirst = first._tcFsAfter;
                block._tcFsLast = last._tcFsBefore;
                block._tcPpsFirst = first._tcPps;
                block._tcPpsLast = last._tcPps;
                block._sCount = uint32_t(count);
                block._nSec = uint16_t(_intervals % _pairsPerFile);
                if (_stream->beginBlock(block))
                {
                    _remaining = uint32_t(count);
                }
                else
                {
                    _failed.store(true);
                }
            }
        }
        if (!getFailed() && _remaining)
        {
            // Copy no more than the interval remainder or the physical ring end.
            // submitSamples copies into a different owned ring before returning;
            // only then may release let the decoder reuse these staging slots.
            const uint32_t position = _tail.load(std::memory_order_relaxed) & (_capacity - 1);
            const uint32_t count = std::min({_remaining, available, _capacity - position, uint32_t(4096)});
            if (count && _stream->submitSamples(_memory + size_t(position) * 3, count))
            {
                release(count);
                _remaining -= count;
                _samples += count;
                retVal = true;
                if (!_remaining)
                {
                    ++_intervals;
                    _boundaryTail.store(iBoundary + 1, std::memory_order_release);
                }
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
 * finish - After decoder and timing submissions stop, require every closed
 * interval to have been pumped. A final open interval is deliberately omitted.
 * This queues the stream's end marker only for a fault-free complete prefix.
 */
bool TimedIntervalBuffer::finish()
{
    const bool retVal = !getFailed() && !_remaining && _intervals > 0 &&
        _boundaryHead.load() - _boundaryTail.load() == 1 && _stream->finish();
    _failed.store(!retVal);
    return retVal;
}

/* **
 * getFailed - All three callers may inspect the latched atomic fault flag.
 */
bool TimedIntervalBuffer::getFailed() const
{
    return _failed.load();
}

/* **
 * getIntervals - Formatting-task count; read elsewhere only after completion.
 */
uint64_t TimedIntervalBuffer::getIntervals() const
{
    return _intervals;
}

/* **
 * getSamples - Formatter-owned count of retained samples submitted to the stream,
 * excluding startup/shutdown partial intervals. Read from another task only
 * after formatting stops. This count alone does not confirm an SD write.
 */
uint64_t TimedIntervalBuffer::getSamples() const
{
    return _samples;
}

/* **
 * getPeak - Largest occupied staging sample count observed by the decoder.
 */
uint32_t TimedIntervalBuffer::getPeak() const
{
    return _peak.load();
}
