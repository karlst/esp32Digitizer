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
 * @file gpio_struct.h
 * @brief Desktop GPIO register storage; tests simulate write-one-to-clear behavior.
 */
#pragma once
#include <cstdint>

/** @brief The three GPIO registers required by the interrupt reader. */
struct fakeGpioRegisters
{
    uint32_t in = 0;
    uint32_t status = 0;
    uint32_t status_w1tc = 0;
};
inline fakeGpioRegisters GPIO;
