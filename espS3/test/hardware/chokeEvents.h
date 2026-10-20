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
 * @file chokeEvents.h
 * @brief Bounded USB diagnostic queue for the temporary Choke test only.
 */
#pragma once
#if S3_CHOKE_TEST
#include <Arduino.h>
#include <freertos/FreeRTOS.h>

/**
 * @brief Copy measurements on working tasks; format and print on the USB task.
 * Acquisition on core 0 and storage on core 1 append under a short memory-only
 * lock. Neither waits for USB. Reserve 16 of 64 slots for transitions rather
 * than repetitive slow writes. Any dropped events are explicitly counted.
 */
class chokeEvents
{
public:
    static void beginRun();
    static void add(const char* name, const char* fields, uint64_t a = 0,
        uint64_t b = 0, uint64_t c = 0, uint64_t d = 0, bool important = true);
    static void drain();
private:
    /**
     * @brief Literal labels plus four measurements captured at the event time.
     * ms starts at the Start request, including preparation. generator-start
     * marks the separate beginning of the ten-second rate ramp.
     */
    struct event
    {
        const char* name;
        const char* fields;
        uint64_t ms, a, b, c, d;
        uint32_t run;
    };
    static portMUX_TYPE lock;
    static event queue[64];
    static uint32_t head, count, run, dropped;
    static uint64_t epochUs;
};
#endif
