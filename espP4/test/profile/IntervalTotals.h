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

/** @file IntervalTotals.h
 * @brief IntervalTotals declaration shared by its callers.
 */
#pragma once
#include "profileMath.h"
namespace profileMath
{
/**
 * @brief Add measured execution and elapsed times, rather than averaging percentages.
 * For example, a busy 0.1-second interval must not count as much as an idle
 * one-second interval. Dividing the summed times gives the proper average.
 * The final partial interval counts in averages, but intervals below 0.5 seconds
 * do not set the worst-window figure. This avoids a tiny Stop interval dominating
 * the estimate through scheduler tick/accounting boundary effects.
 */
struct IntervalTotals
{
    uint64_t _elapsedUs = 0;
    uint64_t _executedUs = 0;
    uint32_t _maxTenths = 0;
    uint32_t _peakWindows = 0;

    /**
     * @brief Add elapsed and executed microseconds for one accepted interval.
     * peakWindows counts all intervals eligible for the maximum (at least
     * 0.5 seconds), not just intervals that set a new maximum.
     */
    void add(uint64_t durationUs, uint64_t runtimeUs)
    {
        _elapsedUs += durationUs;
        _executedUs += runtimeUs;
        if (durationUs >= 500000)
        {
            const uint32_t value = percentTenths(runtimeUs, durationUs);
            if (value > _maxTenths) { _maxTenths = value; }
            ++_peakWindows;
        }
    }
};
}

