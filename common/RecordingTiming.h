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

/** @file RecordingTiming.h
 * @brief Shared limits for preparing storage before ADC sampling begins.
 */
#pragma once
#include <cstdint>
namespace recordingTiming
{
    // P4 supervises preparation for three minutes. Feather allows an extra
    // five seconds for P4's failure reply. Neither duration is recording time.
    constexpr uint32_t preparationTimeoutMs = 180000;
    constexpr uint32_t readyWaitMs = preparationTimeoutMs + 5000;
}
