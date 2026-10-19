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

/** @file ramWaveform.h
 * @brief Bounded waveform summaries, independent of SD and hardware drivers.
 * Core 1 skips one second, then summarizes 300 ms into 600 half-millisecond
 * bins. Every sample contributes to its bin's mean and extrema; this is not
 * an anti-aliased spectrum. Core 0 reads bins only after the decoder drains.
 */
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

/** @brief One 0.5 ms interval; extrema retain excursions hidden by averaging. */
struct waveformBin
{
    int32_t minimum = 8388607;
    int32_t maximum = -8388608;
    int64_t sum = 0;
};

/** @brief Fixed RAM capture with an atomic completion flag shared across cores. */
class ramWaveform
{
public:
    static constexpr unsigned binCount = 600;
    waveformBin bins[binCount];
    uint32_t samplesPerBin = 0;
    uint32_t completedBins = 0;
    void reset(uint32_t rate);
    void accept(const uint32_t* words, size_t count);
    bool complete() const;
private:
    uint32_t skip = 0;
    uint32_t inBin = 0;
    std::atomic<bool> ready{false};
};
