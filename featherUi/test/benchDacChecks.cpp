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

/** @file benchDacChecks.cpp
 * @brief PC checks of the exact DAC code calculation, without hardware access.
 * Code bounds and periodicity do not establish physical output voltage or timing.
 */
#include "benchDacWave.h"
#include <cassert>
#include <cstdio>

/** @brief Check phase, nominal range, repeatability and delayed-update behavior. */
int main()
{
    assert(benchDacWave::codeAt(0) == 97);
    assert(benchDacWave::codeAt(25000) == 135);
    assert(benchDacWave::codeAt(75000) == 58);
    for (uint64_t time = 0; time < benchDacWave::periodUs; time += benchDacWave::updateUs)
    {
        const uint8_t code = benchDacWave::codeAt(time);
        assert(code >= 58 && code <= 135);
        assert(code == benchDacWave::codeAt(time + benchDacWave::periodUs));
        assert(code == benchDacWave::codeAt(time + 1000000000000ULL));
    }
    assert(benchDacWave::codeAt(100000 + 25000) == 135);
    std::puts("PASS: 10 Hz DAC phase, codes 58..135, long-uptime periodicity, skipped-update phase");
    return 0;
}
