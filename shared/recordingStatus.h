/**
 * @file recordingStatus.h
 * @brief Recording values shared by S3 storage and Feather's status receiver.
 * UART version 4 appends these numeric fields to the existing acquisition frame.
 * Positions are byte offsets; totals and times belong to the current/last recording.
 */
#pragma once
#include <cstdint>
#include <cstddef>

/**
 * @brief One consistent snapshot of the card, file writer and sample ring buffer.
 * Only S3 measures these values. Feather must never invent zero for unavailable
 * firmware or a disconnected board. bytesWritten counts successful file writes,
 * including headers; samplesWritten counts only complete sample payloads.
 */
struct recordingStatus
{
    // State: idle, preparing, recording, saving, saved, failed, deleting.
    // Card: unknown, ready, missing, unsupported filesystem, I/O failure.
    enum stateCode : uint32_t { idle, preparing, recording, saving, saved, failed, deleting };
    enum cardCode : uint32_t { unknown, ready, missing, unsupported, ioFailure };
    // Wire representation: 24 unsigned integers in exactly enum field order.
    // No pointers, filename text, or file handles cross between the boards.
    // state/card are numeric labels, enabled is the current/last Start choice.
    // session/part identify the file; elapsedMs is recording-relative time.
    // bytesWritten includes header rewrites; samplesWritten counts payload only.
    // cardBytes/freeBytes describe filesystem allocation space, not ring memory.
    // bufferBytes/usedBytes/peakBytes and read/write positions describe RAM bytes.
    // bytesPerSecond is recent payload throughput. Delay fields ending Us use
    // microseconds; maxDelayAtMs uses milliseconds since recording preparation.
    // maxDelayKind: 0 none, 1 write/preparation, 2 flush/finalization.
    // overflows is a per-recording 0/1 incident flag in the current writer.
    // lostSamples counts explicitly rejected/unsaved records, not unseen ADC events.
    // writeErrors counts failed measured operations; deletedFiles counts successful
    // removals in the latest delete request, including progress before any failure.
    uint64_t values[24] = {};
    // Numeric field indexes are shared so the two boards cannot quietly disagree
    // about column order. A fixed session/part pair generates the safe filename.
    enum field : size_t
    {
        state, card, enabled, session, part, elapsedMs, bytesWritten, samplesWritten,
        cardBytes, freeBytes, bufferBytes, usedBytes, peakBytes, writePosition,
        readPosition, bytesPerSecond, latestDelayUs, maxDelayUs, maxDelayAtMs,
        maxDelayKind, overflows, lostSamples, writeErrors, deletedFiles
    };
    static constexpr size_t fieldCount = 24;
    /**
     * @brief Return the JSON key for one numeric UART column.
     * recordingMonitor uses this to convert a numeric wire column into a browser
     * JSON property. index must be less than fieldCount; this internal helper does
     * not check bounds. The returned name points to permanent text, not new memory.
     */
    static const char* fieldName(size_t index)
    {
        static const char* const names[fieldCount] = {
            "state", "card", "enabled", "session", "part", "elapsedMs", "bytesWritten", "samplesWritten",
            "cardBytes", "freeBytes", "bufferBytes", "usedBytes", "peakBytes", "writePosition",
            "readPosition", "bytesPerSecond", "latestDelayUs", "maxDelayUs", "maxDelayAtMs",
            "maxDelayKind", "overflows", "lostSamples", "writeErrors", "deletedFiles"
        };
        const char* retVal = names[index];
        return retVal;
    }

    /**
     * @brief Reject impossible combinations before exposing a received snapshot.
     * Counts are unsigned 64-bit; positions and capacity are restricted to 32-bit
     * aligned byte offsets. Empty buffers use zero positions, never division by zero.
     *
     * Feather calls this on a temporary parsed frame BEFORE accepting its values.
     * It rejects impossible states and buffer geometry; it does not confirm that the
     * card is physically present or that S3's counters are truthful measurements.
     * The % 4 rules belong to today's four-byte sample format. A future packed format
     * must deliberately update both boards' validation rather than silently fail.
     * Full and empty rings may both have equal read/write offsets; usedBytes tells
     * which one it is. We therefore check position bounds, not require read < write.
     */
    bool valid() const
    {
        // Use a short local name for the same array; no copy is made. All values
        // are already unsigned, so validation focuses on limits and relationships.
        const auto* v = values;
        const bool retVal = v[state] <= deleting && v[card] <= ioFailure && v[enabled] <= 1 &&
            v[session] <= UINT32_MAX && v[part] <= UINT32_MAX && v[maxDelayKind] <= 2 &&
            v[freeBytes] <= v[cardBytes] && v[bufferBytes] <= UINT32_MAX &&
            v[usedBytes] <= v[peakBytes] && v[peakBytes] <= v[bufferBytes] &&
            v[bufferBytes] % 4 == 0 && v[usedBytes] % 4 == 0 &&
            v[writePosition] % 4 == 0 && v[readPosition] % 4 == 0 &&
            (v[bufferBytes] ? v[writePosition] < v[bufferBytes] && v[readPosition] < v[bufferBytes] :
                v[writePosition] == 0 && v[readPosition] == 0);
        return retVal;
    }
};
