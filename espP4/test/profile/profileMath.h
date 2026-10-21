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


}
