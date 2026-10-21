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

/** @file ReceiverStats.h
 * @brief Frozen measurements of raw data received during one recording.
 */
#pragma once
#include <cstdint>

/**
 * @brief Read only after AdcReceiver::stop; these are software observations.
 * Bytes include rejected chunks. Times are microseconds at callback execution,
 * not ADC sample times. Zero errors does not prove that DMA lost no data.
 */
struct ReceiverStats
{
    uint64_t _rawBytes = 0;
    uint32_t _firstBlockBytes = 0;
    uint32_t _callbacks = 0;
    int64_t _firstBlockAt = 0;
    int64_t _lastBlockAt = 0;
    uint32_t _queueHighWater = 0;
    int64_t _maxCallbackGapUs = 0;
    int64_t _maxCopyUs = 0;
    uint32_t _droppedBlocks = 0;
    uint32_t _callbackErrors = 0;
};
