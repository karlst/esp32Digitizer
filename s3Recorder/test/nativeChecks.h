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
 * @file nativeChecks.h
 * @brief Controlled access for finite worker/link tests without an RTOS thread.
 */
#pragma once

/** @brief Let desktop tests call finite private steps instead of launching forever loops.
 * acquisition and featherLink declare this class a friend. That provides test
 * access without exposing internal execute/collect/report methods in their public
 * API. Its methods are defined in nativeChecks.cpp, never linked into firmware. */
class nativeChecks
{
public:
    static void checkAcquisition();
    static void checkLink();
    static void checkFastAcquisition();
    static void checkRecording();
    static void checkChoke();
};
