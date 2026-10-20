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

/** @file hardwareSmokeChecks.cpp
 * @brief PC-only checks of real disk-byte parsing and square-wave timing.
 * This does not exercise SD hardware, the DAC, or ADC voltage accuracy. Karl
 * runs the test runner before building the two opt-in firmware environments.
 */
#include "sdSmokeSummary.h"
#include "benchDacWave.h"
#include "test/support/ads127BenchTiming.h"
#include <cassert>
#include <cstdio>

/** @brief Check byte splits, sign, full duration, and rejection of incomplete tails. */
int main()
{
    assert(ads127BenchTiming::runMs == 30000);
    assert(ads127BenchTiming::receiverTimeoutMs == 35000);
    assert(benchDacWave::codeAt(0) == 110);
    assert(benchDacWave::codeAt(999999) == 110);
    assert(benchDacWave::codeAt(1000000) == 135);
    assert(benchDacWave::codeAt(2000000) == 110);
    assert(benchDacWave::codeAt(29999999) == 135);

    // One sample per bin exposes sign extension and two separate read splits.
    sdSmokeSummary summary;
    summary.reset(10);
    const uint8_t signedValues[] = {255,255,255, 0,0,128, 255,255,127, 1,0,0};
    summary.accept(signedValues, 1);
    summary.accept(signedValues + 1, 3);
    summary.accept(signedValues + 4, 8);
    assert(summary.sampleCount() == 4);
    assert(summary.mean(0) == -1);
    assert(summary.mean(1) == -8388608);
    assert(summary.mean(2) == 8388607);
    assert(summary.mean(3) == 1);
    assert(!summary.complete());

    // Feed a complete 30-second file in awkward 4093-byte reads (not divisible
    // by three). This crosses both sample and 100-ms bin boundaries repeatedly.
    summary.reset(1024000);
    uint8_t block[4093] = {};
    const uint64_t bytes = uint64_t{1024000} * 30 * 3;
    for (uint64_t offset = 0; offset < bytes;)
    {
        const size_t amount = bytes - offset < sizeof(block)
            ? static_cast<size_t>(bytes - offset) : sizeof(block);
        for (size_t index = 0; index < amount; ++index)
        {
            const uint64_t position = offset + index;
            const uint32_t code = ((position / 3) / 1024000) % 2 == 0 ? 1000000 : 2000000;
            block[index] = static_cast<uint8_t>(code >> (8 * (position % 3)));
        }
        summary.accept(block, amount);
        offset += amount;
    }
    assert(summary.complete());
    assert(summary.sampleCount() == 30720000);
    for (unsigned index = 0; index < 300; ++index)
    {
        assert(summary.mean(index) == (index / 10 % 2 == 0 ? 1000000 : 2000000));
    }
    summary.accept(block, 1);
    assert(!summary.complete());
    summary.reset(1024000);
    assert(summary.sampleCount() == 0 && !summary.complete());
    std::puts("PASS: SD byte boundaries, signed samples, duration, square-wave timing and incomplete-file rejection");
    return 0;
}
