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

/** @file OutputCheck.h
 * @brief OutputCheck declaration shared by its callers.
 */
#pragma once
#include <cstdint>
#include <vector>

/** @brief Test-owned copy of submitted words, with an optional rejected batch. */
struct OutputCheck
{
    std::vector<uint32_t> _words;
    std::vector<uint8_t> _packed;
    unsigned _calls = 0;
    unsigned _rejectCall = 0;
};
