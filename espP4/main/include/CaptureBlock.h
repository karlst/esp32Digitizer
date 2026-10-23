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

/**
 * @file CaptureBlock.h
 * Private raw storage retained while the decoder reads it.
 */
#pragma once
#include <cstddef>
#include <cstdint>

/**
 * One raw block copied by Core 0 and retained until Core 1 finishes decoding it.
 * length is the number of valid bytes, not the array capacity. arrivedAt is
 * the interrupt callback's timestamp in microseconds since boot; it lets the
 * decoder measure how long the block waited before being processed.
 */
struct CaptureBlock
{
    size_t _length;
    int64_t _arrivedAt;
    uint8_t _data[4096];
};
