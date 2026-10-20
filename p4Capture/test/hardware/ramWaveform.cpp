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

/** @file ramWaveform.cpp
 * @brief Retain waveform shape without disk I/O or printing during capture.
 */
#include "ramWaveform.h"
#include <cstdio>

/** @brief Core 0 resets while decoder is idle; supported rates divide by 2000.
 * One second of decoded samples is skipped for analog startup settling.
 * Queue handoff subsequently gives Core 1 access to the initialized state.
 */
void ramWaveform::reset(uint32_t rate)
{
    ready.store(false);
    skip = rate;
    samplesPerBin = rate / 2000;
    inBin = completedBins = 0;
    for (auto& bin : bins) { bin = waveformBin{}; }
}

/** @brief Core 1 receives decoder batches; strip status and sign-extend bit 23.
 * The status byte is still checked by frameDecoder. Once the requested window
 * is full, discard later samples while Core 0 coordinates a normal ADC stop.
 * No pointer to the decoder's reusable batch is retained.
 */
void ramWaveform::accept(const uint32_t* words, size_t count)
{
    for (size_t index = 0; index < count && completedBins < binCount; ++index)
    {
        if (skip != 0) { --skip; }
        else if (samplesPerBin != 0)
        {
            const uint32_t raw = words[index] & 0xffffffU;
            const int32_t value = static_cast<int32_t>(raw) -
                ((raw & 0x800000U) ? 0x1000000 : 0);
            auto& bin = bins[completedBins];
            if (value < bin.minimum) { bin.minimum = value; }
            if (value > bin.maximum) { bin.maximum = value; }
            bin.sum += value;
            if (++inBin == samplesPerBin)
            {
                inBin = 0;
                ++completedBins;
                if (completedBins == binCount) { ready.store(true); }
            }
        }
    }
}

/** @brief Core 0 polls only this atomic flag until the decoder has drained. */
bool ramWaveform::complete() const
{
    return ready.load();
}

/**
 * @brief Print the captured groups to USB after the caller has drained decoding.
 * run identifies this capture, rate is samples/second, and clean describes the
 * receiver's error checks. It does not establish analog accuracy. Core 0 calls
 * this only after Core 1 stops changing the bins; no lock is taken here.
 */
void ramWaveform::report(unsigned long run, uint32_t rate, bool clean) const
{
    printf("WAVE_BEGIN run=%lu rate=%lu bins=%lu samplesPerBin=%lu clean=%u\n",
           run, static_cast<unsigned long>(rate),
           static_cast<unsigned long>(completedBins),
           static_cast<unsigned long>(samplesPerBin), unsigned(clean && complete()));
    for (unsigned index = 0; index < completedBins; ++index)
    {
        const auto& bin = bins[index];
        printf("WAVE,%u,%ld,%ld,%.3f\n", index, static_cast<long>(bin.minimum),
               static_cast<long>(bin.maximum), double(bin.sum) / samplesPerBin);
    }
    printf("WAVE_END run=%lu\n", run);
    fflush(stdout);
}
