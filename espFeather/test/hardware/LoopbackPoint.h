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

/** @file LoopbackPoint.h
 * @brief LoopbackPoint declaration shared by its callers.
 */
#pragma once
#include <cstdint>
/** @brief One physical A2 measurement; time is microseconds after sampling begins. */
struct LoopbackPoint
{
    uint32_t _timeUs;
    int _raw;
};
