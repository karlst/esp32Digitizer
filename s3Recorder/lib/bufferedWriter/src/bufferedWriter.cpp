/**
 * @file bufferedWriter.cpp
 * @brief Copy quickly into RAM, then write whole blocks from the storage task.
 * Read start(), submit(), pump(), finish() in that order. No sample or board
 * knowledge belongs here; byteSink supplies storage and the caller supplies time.
 *
 * A ring buffer is a fixed array reused in a circle. It temporarily holds data
 * while the SD card is busy. The producer ADDS bytes; the consumer REMOVES them
 * only after writing them. In this project:
 * ADC -> sampleFormatter::submit() -> bufferedWriter::submit() -> RAM array
 * recordingService::run() -> bufferedWriter::pump() -> sdRecordingSink -> card
 *
 * head is the total producer position, tail the total consumer position. They
 * keep advancing even when the physical array position wraps back to zero.
 * head==tail means empty; head-tail==capacity means full. Keeping the running
 * counts distinguishes those cases even though their array indexes can match.
 * Unsigned subtraction remains valid when the 32-bit counters wrap, provided
 * their distance never exceeds the ring capacity (submit enforces that limit).
 */
#include "bufferedWriter.h"
#include <algorithm>

/**
 * @brief Reset one recording while no producer is running.
 * destination and clock must outlive the recording. The clock returns monotonic
 * microseconds. The adapter may open after this call to measure opening time,
 * but must not enable the producer until the file is ready.
 *
 * Called by recordingService's disk task when preparing a NEW recording.
 * This only prepares RAM; it does not open a file, start the ADC, or erase a card.
 * Resetting positions makes old bytes irrelevant without clearing the whole array.
 * @param destination Object that implements the actual write/flush operations.
 * @param clock Function returning a steadily increasing time in microseconds.
 * The writer borrows both; it does not construct or delete either one.
 */
void bufferedWriter::start(byteSink& destination, uint64_t (*clock)())
{
    // Keep the storage adapter and clock for later calls; allocate no per-sample memory.
    sink = &destination;
    clockUs = clock;
    // Both counters restart together. Neither producer nor consumer may be using
    // the previous recording when this reset happens.
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
 *
 * Called once per formatted sample: from fastCapture's core-0 interrupt at 30k,
 * or from acquisition's core-0 task at lower rates. There must be ONE producer,
 * not both paths at once. The disk task may be reading the ring simultaneously.
 * @param data Bytes to copy. The caller may reuse its memory when this returns.
 * @param length Number of BYTES, not number of samples (currently four per sample).
 * @return True means copied into RAM, NOT saved to disk. False means none of this
 * submission was copied. Acquisition must observe failed(), stop collecting, and
 * ask the disk task to finish the earlier data; this function cannot stop the ADC.
 * No waiting is allowed here: the next 30k sample arrives about 33 microseconds
 * later. An ISR is an interrupt service routine, which briefly interrupts a task.
 */
bool BUFFERED_IRAM bufferedWriter::submit(const uint8_t* data, uint32_t length)
{
    // head belongs to this producer. relaxed is sufficient for our own position.
    // The other core owns tail; acquire means observe space only after that core
    // has finished using it. These are atomic memory operations, not mutex locks.
    const uint32_t writeAt = head.load(std::memory_order_relaxed);
    const uint32_t occupied = writeAt - tail.load(std::memory_order_acquire);
    // head - tail is bytes occupied; capacity - occupied is bytes still free.
    // Seeing an older tail can make us conservative about free space, never unsafe.
    const bool retVal = !broken.load(std::memory_order_relaxed) && length <= capacity - occupied;
    if (retVal)
    {
        // A byte loop also supports future packed formats and avoids a flash-
        // resident memcpy call from an interrupt. Publish only after the copy.
        for (uint32_t index = 0; index < length; ++index)
        {
            // The array is circular: after byte 65535, use byte 0 again. For a
            // power-of-two capacity, AND with 65535 is the same as modulo 65536.
            buffer[(writeAt + index) & (capacity - 1)] = data[index];
        }
        if (occupied + length > peak.load(std::memory_order_relaxed))
        {
            peak.store(occupied + length, std::memory_order_relaxed);
        }
        // Only now advertise the new bytes. release ensures their contents become
        // visible to the consumer BEFORE it sees the advanced head position.
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
 *
 * Only the disk task calls this, immediately after an operation finishes.
 * @param beganUs Clock value captured BEFORE the operation, in microseconds.
 * @param kind 1 for a write/preparation operation; 2 for flush/final close.
 * @param success Whether that operation reported success. False increments the
 * error count and sets the cross-core failure flag; it does not retry the operation.
 * maxAtUs records WHEN the slow operation finished, relative to this recording's
 * start. Free-space queries are not measured here; their stalls can fill the ring.
 */
void bufferedWriter::measure(uint64_t beganUs, uint32_t kind, bool success)
{
    const uint64_t now = clockUs();
    // This is wall-clock duration, including time waiting for the card. Clamp
    // the 32-bit display value rather than letting a very long delay wrap to zero.
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
 *
 * THIS IS WHERE WE READ DATA OUT OF THE RING BUFFER.
 * recordingService::run() calls this repeatedly from the disk task on core 1.
 * Reading here does not mean reading the ADC or loading a file from the SD card.
 * It means giving the oldest queued RAM bytes to the object that writes the file.
 *
 * There is no separate ring read() method or second temporary data array:
 * sink->write(buffer + offset, length) reads directly from our owned buffer.
 * It must finish using that memory before returning. While it waits for the card,
 * submit() may add samples elsewhere in the ring, but cannot reuse this block.
 *
 * @param drain False during recording: normally wait for a 4096-byte batch to
 * avoid a separate filesystem call for every four-byte sample. True after Stop:
 * allow the final short batch, even if it contains only one sample.
 * @return False if a disk write failed (now or previously). True can also mean
 * NOTHING was written: for example, only 100 bytes are queued and drain is false.
 * Use failed() to check the separate buffer-overflow flag; pump() deliberately
 * allows earlier good bytes to be saved after the producer has overflowed.
 */
bool bufferedWriter::pump(bool drain)
{
    bool retVal = totals.errors == 0;
    // tail identifies the oldest queued byte. We own tail; head is updated by
    // the producer. acquire ensures we see the copied data behind its new head.
    const uint32_t readAt = tail.load(std::memory_order_relaxed);
    const uint32_t occupied = head.load(std::memory_order_acquire) - readAt;
    if (active && retVal && occupied && (drain || occupied >= 4096))
    {
        // Convert the running byte count to an array index. Example: tail=65532,
        // head=65544 means 12 queued bytes: four at the end, then eight at the start.
        const uint32_t offset = readAt & (capacity - 1);
        // Take the smallest of queued bytes, our 4096-byte batch limit, and bytes
        // remaining before the array ends. A write cannot read across the array end;
        // the next pump call handles the wrapped portion at index zero.
        const uint32_t length = std::min<uint32_t>(std::min<uint32_t>(occupied, 4096), capacity - offset);
        // Start timing immediately before the storage call, including its wait.
        const uint64_t began = clockUs();
        // This call reads the ring memory and writes the file. sink is a byteSink
        // interface; in this project the actual function is sdRecordingSink::write().
        // It can block on storage. Crucially, tail has NOT moved yet.
        const size_t written = sink->write(buffer + offset, length);
        // Count only the byte count the destination reported accepting. A short
        // write can leave part of a sample on disk; never call that a complete block.
        totals.bytes += std::min<size_t>(written, length);
        retVal = written == length;
        if (!retVal)
        {
            // Keep track of the prefix already accepted, but leave this whole block
            // occupied. Retrying it would write that prefix twice and corrupt ordering.
            partialBytes = static_cast<uint32_t>(std::min<size_t>(written, length));
        }
        measure(began, 1, retVal);
        if (retVal)
        {
            // The entire block was accepted. Release its memory for reuse; acquire
            // in submit() pairs with this store. No shifting/copying of remaining data
            // is necessary: moving the position is enough to remove bytes from the queue.
            tail.store(readAt + length, std::memory_order_release);
        }
    }
    return retVal;
}

/**
 * @brief Drain remaining data and synchronize storage after the producer stops.
 * False means the recording is incomplete, even if earlier bytes were saved.
 * After a write failure we still attempt flush, but never replay a partial block.
 *
 * Called by the disk task after acquisition has disabled the producer. Otherwise
 * new bytes could keep arriving while we try to empty the queue. pump(true) writes
 * one remaining block at a time until empty or a write has failed. flush() asks
 * the destination to synchronize its cached bytes with storage; it is not a new
 * ADC read. The project adapter closes the file separately after this returns.
 * An overflow makes the final result false even if all EARLIER queued bytes drain.
 * Saved counters and maximum delay remain available for inspection after Stop.
 */
bool bufferedWriter::finish()
{
    while (tail.load() != head.load() && totals.errors == 0)
    {
        pump(true);
    }
    // After draining (or a failed write), try to synchronize what did reach the
    // destination. A flush cannot recover bytes rejected by a full ring.
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
 *
 * recordingService calls this when file opening failed, before any sample was
 * allowed into the ring. It freezes elapsed time for that failed attempt. It does
 * not clear errors or queued bytes and is not an alternative to normal Stop.
 */
void bufferedWriter::cancel()
{
    totals.elapsedUs = clockUs() - startedUs;
    active = false;
}

/**
 * @brief True after overflow or storage failure; safe to check from the producer task.
 * The acquisition task checks this between sample-processing steps. The disk task
 * can set it on an I/O error, and submit() can set it on overflow. It remains true
 * until start() resets the next recording. Checking it performs no filesystem I/O.
 */
bool bufferedWriter::failed() const
{
    const bool retVal = broken.load(std::memory_order_acquire);
    return retVal;
}

/**
 * @brief Gather consumer totals and a conservative picture of the live ring.
 * Called only by the consumer. A producer may add bytes during the copy; the
 * reported positions/used refer to the head read here. Peak can already be newer.
 *
 * The caller is the disk task, not an arbitrary third thread. The ordinary totals
 * are only written by that task, so they need no lock here. Atomic fields are used
 * for information also touched by the producer. recordingService later publishes
 * this result under its own short lock for the communications task.
 * Positions shown to the user wrap within the array; totals.bytes does not wrap
 * with the array. The snapshot includes bytes still owned by an in-progress or
 * failed write. Peak occupancy is conservative: a producer can observe an older
 * tail position if the consumer frees space while a submission is in progress.
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
    // A partial-write prefix is still in occupied memory but was already counted
    // as accepted by the file. Subtract it when reporting unsaved queued bytes.
    retVal.unsavedBytes = retVal.used - partialBytes;
    // After finish/cancel, use the frozen elapsed time. Leaving the page open
    // after Stop must not make the completed recording appear to grow longer.
    if (active)
    {
        retVal.elapsedUs = clockUs() - startedUs;
    }
    return retVal;
}
