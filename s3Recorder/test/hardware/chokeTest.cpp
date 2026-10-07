/**
 * @file chokeTest.cpp
 * @brief Pace a configurable ramp or fixed-rate source, optionally time-limited.
 * Read start() then feed(): elapsed time determines how many words SHOULD have
 * arrived; only that many are offered. We never wait for ring space or silently
 * lower the requested rate when storage cannot keep up.
 */
#include "chokeTest.h"
#include <algorithm>

/**
 * @brief Reset a new run after its recording file has opened successfully.
 * nowUs is a monotonic 64-bit microsecond clock. File-opening time is excluded
 * from the rate schedule. Every Start repeats the selected diagnostic schedule.
 */
void chokeTest::start(uint64_t nowUs)
{
    startedUs = nowUs;
    targetBps = initialBps;
    completedBps = 0;
    elapsedMs = 0;
    samples = 0;
    latestSample = 0;
    result = running;
}

/**
 * @brief Offer the samples due by now, returning false when this run must stop.
 * Called about once per scheduler tick, with no lock around the work. A rejected
 * submit means the ring or disk has failed; it is never retried. The disk task
 * retains its normal ownership and failure accounting.
 *
 * Fractional words carry forward automatically. At 750 kbps the desired rate is
 * 23,437.5 words/s, so rounding each individual tick would lose throughput. Instead
 * integrate the entire schedule and subtract words already accepted by the ring.
 */
bool chokeTest::feed(uint64_t nowUs, bufferedWriter& writer)
{
    bool retVal = result == running;
    if (retVal)
    {
        // Cap the last batch at the deadline: scheduler lateness must not add
        // samples beyond the requested duration. Zero duration has no deadline.
        const uint64_t actualElapsedUs = nowUs - startedUs;
        const uint64_t elapsedUs = durationUs && actualElapsedUs > durationUs ? durationUs : actualElapsedUs;

        // Fixed-rate comparison never advances the ramp; both bus widths receive
        // the same byte rate and sample pattern, without ADC timing differences.
        const uint64_t stage = fixedRate ? 0 : elapsedUs / stageUs;
        elapsedMs = static_cast<uint32_t>(std::min<uint64_t>(elapsedUs / 1000, UINT32_MAX));
        if (stage > (UINT32_MAX - initialBps) / incrementBps)
        {
            // Protect the displayed 32-bit rate from overflow in an implausibly
            // long run. This is a numeric limit, not a claim about card performance.
            result = rateLimit;
            retVal = false;
        }
        else
        {
            targetBps = initialBps + static_cast<uint32_t>(stage) * incrementBps;

            // 250,000 bits/s divided by 32 bits/word = one word per 128 us.
            // Initial multiplier is 3 at 750 kbps, or 80 at 20 Mbps. Add each
            // completed stage's multiplier plus the elapsed part of this stage.
            // This cumulative sum retains fractional words across scheduler ticks.
            const uint64_t initialWeight = initialBps / incrementBps;
            const uint64_t stagesWeight = stage * initialWeight + stage * (stage ? stage - 1 : 0) / 2;

            // Each 250,000 bits/s of fixed rate contributes one word per 128 us.
            // It uses total elapsed time, so crossing ten seconds cannot reset
            // the desired count. The ordinary ramp arithmetic stays unchanged.
            const uint64_t wanted = fixedRate ? elapsedUs * initialWeight / 128 :
                (stageUs * stagesWeight + (elapsedUs % stageUs) * (stage + initialWeight)) / 128;
            uint64_t due = wanted - samples;
            if (due > maxDueSamples)
            {
                result = producerBehind;
                retVal = false;
            }

            // Build deterministic words: a counter wraps through the signed 24-bit
            // range. The fourth byte is proper sign extension, exactly as in ordinary
            // recordings. Explicit shifts make the byte order independent of the CPU.
            while (retVal && due != 0)
            {
                const uint32_t words = static_cast<uint32_t>(std::min<uint64_t>(due, sizeof(block) / 4));
                int32_t last = latestSample;
                for (uint32_t index = 0; index < words; ++index)
                {
                    const uint32_t raw = static_cast<uint32_t>(samples + index) & 0xffffff;
                    last = raw & 0x800000 ? static_cast<int32_t>(raw) - 0x1000000 : static_cast<int32_t>(raw);
                    const uint32_t encoded = static_cast<uint32_t>(last);
                    for (uint32_t byte = 0; byte < 4; ++byte)
                    {
                        block[index * 4 + byte] = static_cast<uint8_t>(encoded >> (8 * byte));
                    }
                }
                retVal = writer.submit(block, words * 4);
                if (retVal)
                {
                    samples += words;
                    latestSample = last;
                    due -= words;
                }
                else
                {
                    result = storageLimit;
                }
            }

            // A completed stage is a ten-second observation, not proof of unlimited
            // endurance. Only credit it once all samples due so far were accepted.
            if (retVal && stage != 0)
            {
                completedBps = targetBps - incrementBps;
            }

            // Only a fully submitted final batch earns normal completion. The
            // acquisition task sees false and drains/closes through stopChoke().
            // Storage failure or generator lateness above keeps its own reason.
            if (retVal && durationUs && elapsedUs >= durationUs)
            {
                completedBps = targetBps;
                result = stopped;
                retVal = false;
            }
        }
    }
    return retVal;
}

/**
 * @brief Freeze the outcome while leaving the rate, count and elapsed time visible.
 * The caller stops producing BEFORE asking storage to drain and close the file.
 * This changes no ring memory and performs no I/O.
 */
void chokeTest::stop(resultCode reason)
{
    result = reason;
}
