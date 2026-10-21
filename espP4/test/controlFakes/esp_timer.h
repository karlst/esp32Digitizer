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

/** @file esp_timer.h
 * @brief Test-controlled clock and operation log; no real-time waits.
 */
#pragma once
#include <cstdint>
#include <string>
namespace controlFake
{
inline int64_t nowUs = 1;
inline std::string operations;
}

/** @brief Return the test-selected time in microseconds, preserving the SDK API name. */
inline int64_t esp_timer_get_time() { return controlFake::nowUs; }
