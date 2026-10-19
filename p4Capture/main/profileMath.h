/**
 * @file profileMath.h
 * @brief Portable interval accounting, independent of ESP-IDF and board hardware.
 * Runtime counters measure scheduled execution, not instruction counts. Each
 * core has its own 100% budget: do not divide a core's idle time by TWO cores.
 * Tests exercise unequal intervals and boundary cases without running firmware.
 */
#pragma once
#include <cstdint>

namespace profileMath
{
/**
 * @brief Convert execution microseconds into tenths of a percent of ONE core.
 * Zero duration has no measurable percentage. Clamp minor snapshot/accounting
 * boundary overshoots to 100%; callers separately report those observations.
 * A floating intermediate avoids overflowing a long integer runtime times 1000;
 * returned/displayed values are integer tenths, rounded down.
 */
inline uint32_t percentTenths(uint64_t executedUs, uint64_t elapsedUs)
{
    uint32_t retVal = 0;
    if (elapsedUs != 0)
    {
        retVal = executedUs >= elapsedUs ? 1000 :
            static_cast<uint32_t>((static_cast<long double>(executedUs) * 1000) / elapsedUs);
    }
    return retVal;
}

/**
 * @brief Accumulate weighted execution and meaningful-window maxima.
 * The final partial interval counts in averages, but intervals below 0.5 seconds
 * do not set the worst-window figure. This avoids a tiny Stop interval dominating
 * the estimate through scheduler tick/accounting boundary effects.
 */
struct intervalTotals
{
    uint64_t elapsedUs = 0;
    uint64_t executedUs = 0;
    uint32_t maxTenths = 0;
    uint32_t peakWindows = 0;

    /** @brief Add one valid interval; use actual elapsed time, not assumed 1 s. */
    void add(uint64_t durationUs, uint64_t runtimeUs)
    {
        elapsedUs += durationUs;
        executedUs += runtimeUs;
        if (durationUs >= 500000)
        {
            const uint32_t value = percentTenths(runtimeUs, durationUs);
            if (value > maxTenths) { maxTenths = value; }
            ++peakWindows;
        }
    }
};
}
