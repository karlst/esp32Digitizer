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

/** @file WaveformBin.h
 * @brief WaveformBin declaration shared by its callers.
 */
#pragma once
#include <cstdint>

/** @brief One 0.5 ms interval; extrema retain excursions hidden by averaging. */
struct WaveformBin
{
    int32_t _minimum = 8388607;
    int32_t _maximum = -8388608;
    int64_t _sum = 0;
};
