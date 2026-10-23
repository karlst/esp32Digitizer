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
 * @file CaptureStats.h
 * CaptureStats declaration shared by its callers.
 */
#pragma once
#include <cstdint>

/**
 * Counts of what arrived, not proof that every ADC bit arrived.
 * clocks counts captured clock positions; frames counts FSYNC rising edges;
 * words counts complete 32-bit status+sample values. badPeriods counts wrong
 * distances between frame starts; shortWords counts a new start interrupting
 * an unfinished word. headers counts each possible status byte; badExamples
 * keeps the first eight words whose status failed the mask in finishWord().
 */
struct CaptureStats
{
    uint64_t _clocks = 0;
    uint32_t _frames = 0;
    uint32_t _words = 0;
    uint32_t _badHeaders = 0;
    uint32_t _badPeriods = 0;
    uint32_t _shortWords = 0;
    uint32_t _latestWord = 0;
    int32_t _latestSample = 0;
    uint32_t _headers[256] = {};
    uint32_t _badExamples[8] = {};
};
