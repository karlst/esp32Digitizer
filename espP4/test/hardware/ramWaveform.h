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
 * @brief Keep a short summary of measured voltage samples in RAM, without an SD file.
 * Core 1 skips one second, then summarizes 300 ms into 600 half-millisecond
 * groups (called bins). Every sample contributes to its group's average and
 * minimum/maximum. Individual samples are not retained, and averaging can
 * hide rapid changes. Core 0 reads groups only after queued decoding finishes.
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

/**
 * @brief Core 1 accumulates the summary; Core 0 watches a completion flag.
 * The atomic flag can be read safely across cores. Other fields are ordinary
 * memory: Core 0 resets them before capture and reads them after decoding stops.
 */
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
    void report(unsigned long run, uint32_t rate, bool clean) const;
private:
    uint32_t skip = 0;
    uint32_t inBin = 0;
    std::atomic<bool> ready{false};
};
