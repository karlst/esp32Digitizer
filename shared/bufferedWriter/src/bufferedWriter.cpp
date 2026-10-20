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
 * @file bufferedWriter.cpp
 * @brief Copy quickly into RAM, then write whole blocks from the storage task.
 * Read start(), submit(), pump(), finish() in that order. No sample or board
 * knowledge belongs here; byteSink supplies storage and the caller supplies time.
 *
 * A ring buffer is a fixed array reused in a circle. It temporarily holds data
 * while the SD card is busy. The producer ADDS bytes; the consumer REMOVES them
 * only after writing them. This library is shared by two board adapters:
 * S3: sampleFormatter::submit() -> submit(); recordingService::run() -> pump()
 *     -> sdRecordingSink::write() -> card.
 * P4: Core 1 sdRecorder::submitSamples() -> submit(); Core 0 sdRecorder::work()
 *     -> pump() -> sdRecorder::write() -> card. P4 supplies three-byte samples
 *     and uses a 4 MiB PSRAM ring with 128 KiB write batches. The library itself
 *     treats these as bytes; it does not understand ADC frames or status bits.
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
#include <cstring>

// C++11 needs storage definitions when std::min binds these constants by reference.
constexpr uint32_t bufferedWriter::capacity;
constexpr uint32_t bufferedWriter::blockBytes;

#if BUFFERED_WRITER_EXTERNAL

/**
 * @brief Allocate owned ring memory before any producer or consumer starts.
 * The platform adapter supplies allocate(bytes) and release(pointer). Failure
 * to allocate leaves the writer unusable; callers must not start recording.
 * A true return means memory already exists or was just allocated. Repeated setup
 * reuses the successful allocation, so each Start does not allocate another ring.
 * No ring address is exposed to callers. Desktop tests supply ordinary RAM.
 */
bool bufferedWriter::initializeMemory(uint8_t* (*allocate)(uint32_t), void (*release)(uint8_t*))
{
    bool retVal = buffer != nullptr;
    if (!active && !buffer && allocate && release)
    {
        buffer = allocate(capacity);
        if (buffer)
        {
            releaseMemory = release;
            retVal = true;
        }
    }
    return retVal;
}

/**
 * @brief Release the owned allocation after all users have stopped.
 * Destruction does not flush a file; the owner must finish recording first.
 */
bufferedWriter::~bufferedWriter()
{
    if (buffer && releaseMemory)
    {
        releaseMemory(buffer);
    }
}
#endif

/**
 * @brief Prepare an empty recording buffer while submitting and writing are stopped.
 * destination and clock must outlive the recording. The clock returns monotonic
 * microseconds. The adapter may open after this call to measure opening time,
 * but must not enable the producer until the file is ready.
 *
 * Called by recordingService (S3) or sdRecorder (P4) when preparing a NEW recording.
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
#if BUFFERED_WRITER_EXTERNAL
    broken.store(buffer == nullptr);
#else
    broken.store(false);
#endif
    overflowed.store(false);
    partialBytes = 0;
    totals = {};
    startedUs = clockUs();
    active = true;
}

/**
 * @brief Copy all supplied bytes or reject the entire submission without waiting.
 * A full buffer sets a failure flag until start(); later submissions are rejected too. This avoids
 * silently joining data on either side of a missing section. rejectedBytes counts
 * only bytes explicitly offered and rejected, not unseen hardware samples.
 *
 * S3 calls once per formatted sample from fastCapture's core-0 interrupt at 30k,
 * or acquisition's core-0 task at lower rates. P4 instead calls from its Core 1
 * decoder task with batches of packed three-byte samples (four-byte words for
 * its separate synthetic test). There must be ONE producer, never both paths
 * at once. The disk task may be reading other ring bytes simultaneously.
 * The caller must call start() first and stop submitting before finish() or
 * cancel(). submit() does not check active and cannot enforce that lifecycle.
 * @param data Bytes to copy. The caller may reuse its memory when this returns.
 * @param length Number of BYTES, not samples; the board adapter chooses the format.
 * @return True means copied into RAM, NOT saved to disk. False means none of this
 * submission was copied. Acquisition must observe failed(), stop collecting, and
 * ask the disk task to finish the earlier data; this function cannot stop the ADC.
 * No waiting is allowed here: incoming acquisition continues while disk writes
 * may stall. An ISR is an interrupt service routine, which briefly interrupts a
 * task. P4 submits from a task, not an ISR; its external-memory path uses memcpy.
 */
bool bufferedWriter::submit(const uint8_t* data, uint32_t length)
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
#if BUFFERED_WRITER_EXTERNAL

        // Task-only PSRAM path: copy the span up to the array end, then any
        // wrapped remainder at zero. For example, 12 bytes starting 4 bytes
        // before the end become memcpy lengths 4 and 8. Publish only after both.
        const uint32_t offset = writeAt & (capacity - 1);
        const uint32_t first = std::min(length, capacity - offset);
        std::memcpy(buffer + offset, data, first);
        if (length > first)
        {
            std::memcpy(buffer, data + first, length - first);
        }
#else

        // Internal-RAM ISR path avoids calling a flash-resident memcpy.
        for (uint32_t index = 0; index < length; ++index)
        {
            buffer[(writeAt + index) & (capacity - 1)] = data[index];
        }
#endif
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
 * @brief Write up to blockBytes contiguous bytes; drain also permits a short final block.
 * Only the storage task calls this. Short writes are counted honestly, latch an
 * error, and leave the queued block unreleased; retrying could duplicate its prefix.
 * An overflow still allows earlier queued good bytes to be drained.
 *
 * THIS IS WHERE WE READ DATA OUT OF THE RING BUFFER.
 * S3 recordingService::run() calls this from its Core 1 disk task; P4
 * sdRecorder::work() calls it from its Core 0 disk task. The adapter determines
 * scheduling; this reusable library does not create tasks or choose a CPU core.
 * Reading here does not mean reading the ADC or loading a file from the SD card.
 * It means giving the oldest queued RAM bytes to the object that writes the file.
 *
 * There is no separate ring read() method or temporary array in this library:
 * sink->write(buffer + offset, length) reads directly from our owned buffer.
 * It must finish using that memory before returning. While it waits for the card,
 * submit() may add samples elsewhere in the ring, but cannot reuse this block.
 * An adapter may use temporary storage internally: P4 copies 16 KiB slices to
 * internal RAM before f_write(). That does not release the source ring early.
 *
 * @param drain False during recording: normally wait for a blockBytes-sized batch to
 * avoid a separate filesystem call for every sample. True after Stop:
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
    if (active && retVal && occupied && (drain || occupied >= blockBytes))
    {
        // Convert the running byte count to an array index. For example, twelve
        // bytes starting four bytes before capacity wrap eight bytes to zero.
        const uint32_t offset = readAt & (capacity - 1);

        // Take the smallest of queued bytes, our configured batch limit, and bytes
        // remaining before the array ends. A write cannot read across the array end;
        // the next pump call handles the wrapped portion at index zero.
        const uint32_t length = std::min<uint32_t>(std::min<uint32_t>(occupied, blockBytes), capacity - offset);

        // Start timing immediately before the storage call, including its wait.
        const uint64_t began = clockUs();

        // This call reads the ring memory and writes the file. sink is a byteSink
        // interface; the implementation is sdRecordingSink::write() on S3 or
        // sdRecorder::write() on P4. Both must finish reading before returning.
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

/**
 * @brief Read live occupied bytes on the producer OR consumer, without I/O.
 * Each caller owns one position, so it cannot move during these two loads.
 * Other tasks must use published snapshots. Do not call during start/reset.
 * This observes occupancy; it does not reserve space for a subsequent submit.
 */
uint32_t bufferedWriter::queuedBytes() const
{
    const uint32_t written = head.load(std::memory_order_acquire);
    const uint32_t retVal = written - tail.load(std::memory_order_acquire);
    return retVal;
}
