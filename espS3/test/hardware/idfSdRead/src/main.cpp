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
 * @file main.cpp
 * @brief Run the unchanged read-only comparison using the newer ESP-IDF runtime.
 */
void setup();
void loop();

/**
 * @brief ESP-IDF entry point; call shared startup once and repeat its command loop.
 * The default main task owns every buffer and SD operation. loop() yields using
 * FreeRTOS, and runs a new comparison only when its existing logic requests it.
 */
extern "C" void app_main()
{
    setup();
    for (;;)
    {
        loop();
    }
}
