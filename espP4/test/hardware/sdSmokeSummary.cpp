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

/** @file sdSmokeSummary.cpp
 * @brief Reconstruct SD samples across arbitrary read boundaries and bin them.
 * Read reset(), accept(), complete(), report(). The only input is the recorded
 * three-byte payload, excluding its header. Bin timestamps use the requested
 * sample rate; they are not an independent hardware clock measurement.
 */
#include "sdSmokeSummary.h"
#include <cstdio>

/** @brief Start a new readback; rate is ADC samples per second, not byte rate. */
void sdSmokeSummary::reset(uint32_t samplesPerSecond)
{
    rate = samplesPerSecond;
    samples = clipped = 0;
    partial = partialBytes = 0;
    for (unsigned index = 0; index < 300; ++index)
    {
        sums[index] = 0;
        counts[index] = 0;
    }
}

/** @brief Consume bytes copied from disk; retain only partial word and bin totals.
 * Low byte arrives first. Assemble into an unsigned word, then subtract 2^24
 * if sign bit 23 is set. This avoids relying on implementation-defined shifts.
 * E.g. bytes 00 00 80 become -8388608. Count samples beyond 30 seconds too,
 * although there are only 300 bins; complete() rejects an excessive duration.
 */
void sdSmokeSummary::accept(const uint8_t* data, size_t bytes)
{
    for (size_t index = 0; index < bytes; ++index)
    {
        partial |= static_cast<uint32_t>(data[index]) << (8 * partialBytes);
        ++partialBytes;
        if (partialBytes == 3)
        {
            const int32_t value = (partial & 0x800000U) != 0
                ? static_cast<int32_t>(partial) - 16777216 : static_cast<int32_t>(partial);
            const uint64_t bin = rate != 0 ? samples * 10 / rate : 300;
            if (bin < 300)
            {
                sums[bin] += value;
                ++counts[bin];
            }
            if (value == -8388608 || value == 8388607) { ++clipped; }
            ++samples;
            partial = partialBytes = 0;
        }
    }
}

/** @brief Require stage-6 timing within 20 ms, all bins, and no saturated codes.
 * The DMA receiver can omit its final partial block; 20 ms is a gross duration
 * tolerance, not proof of zero lost samples. CRC and receiver errors are checked
 * separately by the recorder. A disconnected but steady signal can pass these
 * structural checks; the PC must also compare against the measured input.
 */
bool sdSmokeSummary::complete() const
{
    bool retVal = rate == 1024000 && partialBytes == 0 && clipped == 0 &&
        samples >= uint64_t{rate} * 30 - rate / 50 &&
        samples <= uint64_t{rate} * 30 + rate / 50;
    for (unsigned index = 0; index < 300; ++index)
    {
        if (counts[index] < rate * 8 / 100) { retVal = false; }
    }
    return retVal;
}

/** @brief Return an individual disk-bin mean in ADC counts, or zero if absent. */
double sdSmokeSummary::mean(unsigned index) const
{
    const double retVal = index < 300 && counts[index] != 0
        ? static_cast<double>(sums[index]) / counts[index] : 0.0;
    return retVal;
}

/** @brief Return all complete three-byte samples read, including any excess tail. */
uint64_t sdSmokeSummary::sampleCount() const
{
    return samples;
}

/** @brief Print disk evidence after final file sync/close; never print during capture.
 * clean includes the caller's storage and acquisition result. The PC still
 * checks run IDs, byte counts, input voltage and each settled bin before PASS.
 */
void sdSmokeSummary::report(uint32_t run, bool storageClean) const
{
    std::printf("SMOKE_SD_BEGIN run=%lu rate=%lu samples=%llu bins=300 clean=%u\n",
                static_cast<unsigned long>(run), static_cast<unsigned long>(rate),
                static_cast<unsigned long long>(samples), unsigned(storageClean && complete()));
    for (unsigned index = 0; index < 300; ++index)
    {
        std::printf("SMOKE_SD,%u,%lu,%.6f\n", index,
                    static_cast<unsigned long>(counts[index]), mean(index));
    }
    std::printf("SMOKE_SD_END run=%lu\n", static_cast<unsigned long>(run));
}
