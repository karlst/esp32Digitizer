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
 * @file esp_system.h
 * @brief Replace the hardware random-number function with a repeatable test seed.
 * Production firmware uses esp_random() from the real ESP framework so command
 * sequences differ across boots. This fake only makes desktop assertions predictable.
 */
#pragma once
/**
 * @brief Return a known starting ID so acknowledgement tests are reproducible.
 */
inline uint32_t esp_random() { return 100; }
