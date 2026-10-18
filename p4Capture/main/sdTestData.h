/** @file sdTestData.h
 * @brief Hardware-independent timing and data pattern for the SD-only test.
 */
#pragma once
#include <cstdint>

namespace sdTestData
{
constexpr uint64_t durationUs = 60000000;
constexpr uint32_t stage = 0xffffffffU;

/** @brief Number of four-byte words due by elapsed microseconds, capped at 60 s.
 * Integer arithmetic avoids accumulated rounding from individual task sleeps.
 * The two selected rates are 812500 and 1024000 words/s (26 and 32.768 Mbps).
 */
inline uint64_t dueWords(uint64_t elapsedUs, uint32_t wordsPerSecond)
{
    const uint64_t limited = elapsedUs < durationUs ? elapsedUs : durationUs;
    return limited * wordsPerSecond / 1000000;
}

/** @brief Deterministic full-width word; file readback independently checks each index. */
inline uint32_t word(uint32_t index)
{
    return (index * 1664525U + 1013904223U) ^ 0xa57c391eU;
}
}
