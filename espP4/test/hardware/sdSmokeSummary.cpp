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

/** @file SdSmokeSummary.cpp
 * @brief Reconstruct SD samples across arbitrary read boundaries and bin them.
 * Read reset(), accept(), getComplete(), report(). The only input is the recorded
 * three-byte payload, excluding its header. Bin timestamps use the requested
 * sample rate; they are not an independent hardware clock measurement.
 */
#include "SdSmokeSummary.h"
#include <cstdio>

/** @brief Start a new readback; rate is ADC samples per second, not byte rate. */
void SdSmokeSummary::reset(uint32_t samplesPerSecond)
{
    _rate = samplesPerSecond;
    _samples = _clipped = 0;
    _partial = _partialBytes = 0;
    for (unsigned index = 0; index < 300; ++index)
    {
        _sums[index] = 0;
        _counts[index] = 0;
    }
}

/** @brief Consume bytes copied from disk; retain only partial word and bin totals.
 * Low byte arrives first. Assemble into an unsigned word, then subtract 2^24
 * if sign bit 23 is set. This avoids relying on implementation-defined shifts.
 * E.g. bytes 00 00 80 become -8388608. Count samples beyond 30 seconds too,
 * although there are only 300 bins; getComplete() rejects an excessive duration.
 */
void SdSmokeSummary::accept(const uint8_t* data, size_t bytes)
{
    for (size_t index = 0; index < bytes; ++index)
    {
        _partial |= static_cast<uint32_t>(data[index]) << (8 * _partialBytes);
        ++_partialBytes;
        if (_partialBytes == 3)
        {
            const int32_t value = (_partial & 0x800000U) != 0
                ? static_cast<int32_t>(_partial) - 16777216 : static_cast<int32_t>(_partial);
            const uint64_t bin = _rate != 0 ? _samples * 10 / _rate : 300;
            if (bin < 300)
            {
                _sums[bin] += value;
                ++_counts[bin];
            }
            if (value == -8388608 || value == 8388607) { ++_clipped; }
            ++_samples;
            _partial = _partialBytes = 0;
        }
    }
}

/** @brief Require 1,024,000-sample/s timing within 20 ms, all bins, and no saturated codes.
 * The DMA receiver can omit its final partial block; 20 ms is a gross duration
 * tolerance, not proof of zero lost samples. CRC and receiver errors are checked
 * separately by the recorder. A disconnected but steady signal can pass these
 * structural checks; the PC must also compare against the measured input.
 */
bool SdSmokeSummary::getComplete() const
{
    bool retVal = _rate == 1024000 && _partialBytes == 0 && _clipped == 0 &&
        _samples >= uint64_t{_rate} * 30 - _rate / 50 &&
        _samples <= uint64_t{_rate} * 30 + _rate / 50;
    for (unsigned index = 0; index < 300; ++index)
    {
        if (_counts[index] < _rate * 8 / 100) { retVal = false; }
    }
    return retVal;
}

/** @brief Return an individual disk-bin mean in ADC counts, or zero if absent. */
double SdSmokeSummary::getMean(unsigned index) const
{
    const double retVal = index < 300 && _counts[index] != 0
        ? static_cast<double>(_sums[index]) / _counts[index] : 0.0;
    return retVal;
}

/** @brief Return all complete three-byte samples read, including any excess tail. */
uint64_t SdSmokeSummary::getSampleCount() const
{
    return _samples;
}

/** @brief Print disk evidence after final file sync/close; never print during capture.
 * clean includes the caller's storage and acquisition result. The PC still
 * checks run IDs, byte counts, input voltage and each settled bin before PASS.
 */
void SdSmokeSummary::report(uint32_t run, bool storageClean) const
{
    std::printf("SMOKE_SD_BEGIN run=%lu rate=%lu samples=%llu bins=300 clean=%u\n",
                static_cast<unsigned long>(run), static_cast<unsigned long>(_rate),
                static_cast<unsigned long long>(_samples), unsigned(storageClean && getComplete()));
    for (unsigned index = 0; index < 300; ++index)
    {
        std::printf("SMOKE_SD,%u,%lu,%.6f\n", index,
                    static_cast<unsigned long>(_counts[index]), getMean(index));
    }
    std::printf("SMOKE_SD_END run=%lu\n", static_cast<unsigned long>(run));
}
