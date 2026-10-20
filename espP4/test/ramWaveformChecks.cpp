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

/** @file ramWaveformChecks.cpp
 * @brief PC checks of grouping samples into minimum, maximum and average records.
 * This calls the real RAM collector with invented words. No ADC, DMA receiver,
 * timing or inter-core synchronization is exercised.
 */
#include "RamWaveform.h"
#include <cassert>
#include <cstdio>

/** @brief Exercise actual collector with the lowest supported 16k sample rate. */
int main()
{
    // At 16,000 samples/second, the first 16,000 words are the one-second skip.
    // Even full-scale startup values must not enter the retained groups.
    RamWaveform ramWaveform;
    ramWaveform.reset(16000);
    const uint32_t startup = 0x087fffff;
    for (unsigned index = 0; index < 16000; ++index) { ramWaveform.accept(&startup, 1); }
    assert(ramWaveform._completedBins == 0 && !ramWaveform.complete());
    // Eight samples make one 0.5-ms group at this rate. Split each group across
    // two accept() calls to check that a caller's batch boundary cannot reset it.
    const uint32_t batch[] = {0x08800000, 0x087fffff, 0x08ffffff, 0x08000001,
                             0x08000000, 0x08000000, 0x08000000, 0x08000000};
    for (unsigned bin = 0; bin < 600; ++bin)
    {
        ramWaveform.accept(batch, 3);
        assert(ramWaveform._completedBins == bin);
        ramWaveform.accept(batch + 3, 5);
    }
    assert(ramWaveform.complete() && ramWaveform._completedBins == 600);
    assert(ramWaveform._waveformBins[0]._minimum == -8388608 && ramWaveform._waveformBins[599]._maximum == 8388607);
    assert(ramWaveform._waveformBins[0]._sum == -1);
    // A completed 600-group capture ignores later words; reset must clear it
    // and select 512 samples per group for the next, full-rate capture.
    ramWaveform.accept(&startup, 1);
    assert(ramWaveform._waveformBins[599]._sum == -1 && ramWaveform._completedBins == 600);
    ramWaveform.reset(1024000);
    assert(!ramWaveform.complete() && ramWaveform._completedBins == 0 && ramWaveform._samplesPerBin == 512);
    std::puts("PASS: RAM waveform skip, signed samples, split batches, extrema, cap and reset");
    return 0;
}
