/**
 * @file chokeTest.cpp
 * @brief Pace a 750-kbps source, adding 250 kbps every ten seconds of generation.
 * Read start() then feed(): elapsed time determines how many words SHOULD have
 * arrived; only that many are offered. We never wait for ring space or silently
 * lower the requested rate when storage cannot keep up.
 */
#include "chokeTest.h"
#include <algorithm>

/**
 * @brief Reset a new run after its recording file has opened successfully.
 * nowUs is a monotonic 64-bit microsecond clock. File-opening time is excluded
 * from the rate schedule. Every Start repeats the ramp from 750 kbps.
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
        const uint64_t elapsedUs = nowUs - startedUs;
        const uint64_t stage = elapsedUs / stageUs;
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
            // Complete stages use multipliers 3,4,5,...; the arithmetic-series sum
            // below plus the current partial stage gives an exact cumulative target.
            const uint64_t stagesWeight = stage * 3 + stage * (stage ? stage - 1 : 0) / 2;
            const uint64_t wanted = (stageUs * stagesWeight +
                (elapsedUs % stageUs) * (stage + 3)) / 128;
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
