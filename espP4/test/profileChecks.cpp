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
 * @file profileChecks.cpp
 * @brief Host checks for CPU normalization, interval weighting and 60s timing.
 * These do not validate FreeRTOS accounting, interrupt load or hardware capture.
 * Build with ADS127_P4_LONG_RUN=1, as in the profiling firmware.
 */
#include "profileMath.h"
#include "IntervalTotals.h"
#include "test/support/ads127BenchTiming.h"
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
    profileMath::IntervalTotals intervalTotals;
    intervalTotals.add(1000000, 1000000);
    intervalTotals.add(2000000, 0);
    assert(profileMath::percentTenths(intervalTotals._executedUs, intervalTotals._elapsedUs) == 333);
    assert(intervalTotals._maxTenths == 1000 && intervalTotals._peakWindows == 2);

    // A tiny final interval counts toward totals but cannot dominate worst load.
    profileMath::IntervalTotals intervalTotalsTail;
    intervalTotalsTail.add(1000000, 250000);
    intervalTotalsTail.add(1000, 1000);
    assert(intervalTotalsTail._elapsedUs == 1001000 && intervalTotalsTail._executedUs == 251000);
    assert(intervalTotalsTail._maxTenths == 250 && intervalTotalsTail._peakWindows == 1);
    intervalTotalsTail.add(500000, 250000);
    assert(intervalTotalsTail._maxTenths == 500 && intervalTotalsTail._peakWindows == 2);

    // Long uptime must not truncate accumulated runtime to 32 bits.
    assert(profileMath::percentTenths(6000000000ULL, 12000000000ULL) == 500);
    static_assert(ads127BenchTiming::runMs == 60000, "Profile must run for 60 seconds");
    static_assert(ads127BenchTiming::receiverTimeoutMs == 65000, "Profile deadline is 65 seconds");
    std::puts("PASS: per-core percentages, weighted windows, short tail, 64-bit totals, 60/65s timing");
    return 0;
}
