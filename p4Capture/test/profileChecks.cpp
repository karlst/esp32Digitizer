/**
 * @file profileChecks.cpp
 * @brief Host checks for CPU normalization, interval weighting and 60s timing.
 * These do not validate FreeRTOS accounting, interrupt load or hardware capture.
 * Build with ADS127_P4_LONG_RUN=1, as in the profiling firmware.
 */
#include "profileMath.h"
#include "ads127BenchTiming.h"
#include <cassert>
#include <cstdio>

/** @brief Exercise cases that could otherwise overstate spare processor time. */
int main()
{
    // A fully occupied core is 100%, not 50% because the chip has two cores.
    assert(profileMath::percentTenths(1000000, 1000000) == 1000);
    assert(profileMath::percentTenths(250000, 1000000) == 250);
    assert(profileMath::percentTenths(0, 1000000) == 0);
    assert(profileMath::percentTenths(10, 0) == 0);
    assert(profileMath::percentTenths(1001000, 1000000) == 1000);

    // Unequal windows must be duration-weighted: 100% for 1s and 0% for 2s
    // means 33.3%, not the incorrect arithmetic mean of 50%.
    profileMath::intervalTotals totals;
    totals.add(1000000, 1000000);
    totals.add(2000000, 0);
    assert(profileMath::percentTenths(totals.executedUs, totals.elapsedUs) == 333);
    assert(totals.maxTenths == 1000 && totals.peakWindows == 2);

    // A tiny final interval counts toward totals but cannot dominate worst load.
    profileMath::intervalTotals tail;
    tail.add(1000000, 250000);
    tail.add(1000, 1000);
    assert(tail.elapsedUs == 1001000 && tail.executedUs == 251000);
    assert(tail.maxTenths == 250 && tail.peakWindows == 1);
    tail.add(500000, 250000);
    assert(tail.maxTenths == 500 && tail.peakWindows == 2);

    // Long uptime must not truncate accumulated runtime to 32 bits.
    assert(profileMath::percentTenths(6000000000ULL, 12000000000ULL) == 500);
    static_assert(ads127BenchTiming::runMs == 60000, "Profile must run for 60 seconds");
    static_assert(ads127BenchTiming::receiverTimeoutMs == 65000, "Profile deadline is 65 seconds");
    std::puts("PASS: per-core percentages, weighted windows, short tail, 64-bit totals, 60/65s timing");
    return 0;
}
