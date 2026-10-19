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
 * @file diagnosticSerial.h
 * @brief USB-bridge UART logging for the isolated ESP-IDF comparison.
 */
#pragma once
#include <cstddef>

/**
 * @brief Implement the small Serial API used by the shared diagnostic source.
 * Only the main task calls these methods. UART0 reaches COM4 through the board's
 * USB bridge; UART1 and the Feather are never started. This is not an SD adapter.
 */
class diagnosticSerial
{
public:
    void begin(unsigned long baud);
    void printf(const char* format, ...);
    void print(const char* message);
    void println(const char* message = "");
    int available();
    int read();
};
