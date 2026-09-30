/**
 * @file bufferedWriter.cpp
 * @brief Copy quickly into RAM, then write whole blocks from the storage task.
 * Read start(), submit(), pump(), finish() in that order. No sample or board
 * knowledge belongs here; byteSink supplies storage and the caller supplies time.
 */
#include "bufferedWriter.h"
#include <algorithm>

/**
 * @brief Reset one recording while no producer is running.
 * destination and clock must outlive the recording. The clock returns monotonic
 * microseconds. The adapter may open after this call to measure opening time,
 * but must not enable the producer until the file is ready.
 */
void bufferedWriter::start(byteSink& destination, uint64_t (*clock)())
{
    sink = &destination;
    clockUs = clock;
    head.store(0); tail.store(0); peak.store(0); rejected.store(0);
    broken.store(false);
    overflowed.store(false);
    partialBytes = 0;
    totals = {};
    startedUs = clockUs();
    active = true;
}

/**
 * @brief Copy all supplied bytes or reject the entire submission without waiting.
 * A full buffer latches failure; later submissions are rejected too. This avoids
 * silently joining data on either side of a missing section. rejectedBytes counts
 * only bytes explicitly offered and rejected, not unseen hardware samples.
 */
bool BUFFERED_IRAM bufferedWriter::submit(const uint8_t* data, uint32_t length)
{
    const uint32_t writeAt = head.load(std::memory_order_relaxed);
    const uint32_t occupied = writeAt - tail.load(std::memory_order_acquire);
    const bool retVal = !broken.load(std::memory_order_relaxed) && length <= capacity - occupied;
    if (retVal)
    {
        // A byte loop also supports future packed formats and avoids a flash-
        // resident memcpy call from an interrupt. Publish only after the copy.
        for (uint32_t index = 0; index < length; ++index)
        {
            buffer[(writeAt + index) & (capacity - 1)] = data[index];
        }
        if (occupied + length > peak.load(std::memory_order_relaxed))
        {
            peak.store(occupied + length, std::memory_order_relaxed);
        }
        head.store(writeAt + length, std::memory_order_release);
    }
    else
    {
        if (length > capacity - occupied)
        {
            overflowed.store(true, std::memory_order_relaxed);
        }
        // Only this producer changes rejected, so no atomic read/modify/write
        // instruction or interrupt-unsafe library helper is needed.
        rejected.store(rejected.load(std::memory_order_relaxed) + length, std::memory_order_relaxed);
        broken.store(true, std::memory_order_release);
    }
    return retVal;
}

/**
 * @brief Record elapsed time for one write (kind 1) or synchronization (kind 2).
 * The adapter may also measure initial headers, part rollover, and final close.
 * Maxima are kept here until start(), including a slow final flush after Stop.
 */
void bufferedWriter::measure(uint64_t beganUs, uint32_t kind, bool success)
{
    const uint64_t now = clockUs();
    const uint64_t duration = now - beganUs;
    totals.latestUs = static_cast<uint32_t>(std::min<uint64_t>(duration, UINT32_MAX));
    totals.elapsedUs = now - startedUs;
    if (totals.latestUs > totals.maxUs)
    {
        totals.maxUs = totals.latestUs;
        totals.maxAtUs = now - startedUs;
        totals.maxKind = kind;
    }
    if (!success)
    {
        ++totals.errors;
        broken.store(true, std::memory_order_release);
    }
}

/**
 * @brief Write up to 4096 contiguous bytes; drain also permits a short final block.
 * Only the storage task calls this. Short writes are counted honestly, latch an
 * error, and leave the queued block unreleased; retrying could duplicate its prefix.
 * An overflow still allows earlier queued good bytes to be drained.
 */
bool bufferedWriter::pump(bool drain)
{
    bool retVal = totals.errors == 0;
    const uint32_t readAt = tail.load(std::memory_order_relaxed);
    const uint32_t occupied = head.load(std::memory_order_acquire) - readAt;
    if (active && retVal && occupied && (drain || occupied >= 4096))
    {
        const uint32_t offset = readAt & (capacity - 1);
        const uint32_t length = std::min<uint32_t>(std::min<uint32_t>(occupied, 4096), capacity - offset);
        const uint64_t began = clockUs();
        const size_t written = sink->write(buffer + offset, length);
        totals.bytes += std::min<size_t>(written, length);
        retVal = written == length;
        if (!retVal)
        {
            partialBytes = static_cast<uint32_t>(std::min<size_t>(written, length));
        }
        measure(began, 1, retVal);
        if (retVal)
        {
            tail.store(readAt + length, std::memory_order_release);
        }
    }
    return retVal;
}

/**
 * @brief Drain remaining data and synchronize storage after the producer stops.
 * False means the recording is incomplete, even if earlier bytes were saved.
 * After a write failure we still attempt flush, but never replay a partial block.
 */
bool bufferedWriter::finish()
{
    while (tail.load() != head.load() && totals.errors == 0)
    {
        pump(true);
    }
    const uint64_t began = clockUs();
    const bool flushed = sink->flush();
    measure(began, 2, flushed);
    active = false;
    const bool retVal = !failed();
    return retVal;
}

/**
 * @brief Freeze a failed preparation without flushing a file that never opened.
 * Only valid before enabling the producer. Queued data must use finish() instead.
 */
void bufferedWriter::cancel()
{
    totals.elapsedUs = clockUs() - startedUs;
    active = false;
}

/** @brief True after overflow or storage failure; safe to check from the producer task. */
bool bufferedWriter::failed() const
{
    const bool retVal = broken.load(std::memory_order_acquire);
    return retVal;
}

/**
 * @brief Gather consumer totals and a conservative picture of the live ring.
 * Called only by the consumer. A producer may add bytes during the copy; the
 * reported positions/used refer to the head read here. Peak can already be newer.
 */
bufferedWriter::statistics bufferedWriter::snapshot()
{
    statistics retVal = totals;
    const uint32_t readAt = tail.load(std::memory_order_relaxed);
    const uint32_t writeAt = head.load(std::memory_order_acquire);
    retVal.used = writeAt - readAt;
    retVal.peak = std::max(retVal.used, peak.load(std::memory_order_relaxed));
    retVal.readPosition = readAt & (capacity - 1);
    retVal.writePosition = writeAt & (capacity - 1);
    retVal.rejectedBytes = rejected.load(std::memory_order_relaxed);
    retVal.overflows = overflowed.load(std::memory_order_relaxed) ? 1 : 0;
    retVal.unsavedBytes = retVal.used - partialBytes;
    if (active)
    {
        retVal.elapsedUs = clockUs() - startedUs;
    }
    return retVal;
}
