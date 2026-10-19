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
 * @brief PC checks: settling skip, signed extrema, bin boundaries, reset and cap.
 */
#include "ramWaveform.h"
#include <cassert>
#include <cstdio>

/** @brief Exercise actual collector with the lowest supported 16k sample rate. */
int main()
{
    ramWaveform wave;
    wave.reset(16000);
    const uint32_t startup = 0x087fffff;
    for (unsigned index = 0; index < 16000; ++index) { wave.accept(&startup, 1); }
    assert(wave.completedBins == 0 && !wave.complete());
    const uint32_t batch[] = {0x08800000, 0x087fffff, 0x08ffffff, 0x08000001,
                             0x08000000, 0x08000000, 0x08000000, 0x08000000};
    for (unsigned bin = 0; bin < 600; ++bin)
    {
        wave.accept(batch, 3);
        assert(wave.completedBins == bin);
        wave.accept(batch + 3, 5);
    }
    assert(wave.complete() && wave.completedBins == 600);
    assert(wave.bins[0].minimum == -8388608 && wave.bins[599].maximum == 8388607);
    assert(wave.bins[0].sum == -1);
    wave.accept(&startup, 1);
    assert(wave.bins[599].sum == -1 && wave.completedBins == 600);
    wave.reset(1024000);
    assert(!wave.complete() && wave.completedBins == 0 && wave.samplesPerBin == 512);
    std::puts("PASS: RAM waveform skip, signed samples, split batches, extrema, cap and reset");
    return 0;
}
