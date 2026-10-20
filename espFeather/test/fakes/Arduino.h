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
 * @file Arduino.h
 * @brief Desktop substitutes for testing the actual Feather status parser and JSON.
 * Reuse S3's fake clock/serial buffers, then add the small String and HardwareSerial
 * interfaces needed by Feather code. This is not a general Arduino implementation.
 * The test runner's include order selects it only for monitorChecks.exe.
 */
#pragma once
#include "../../../espS3/test/fakes/Arduino.h"
#include <type_traits>
#define RX 7
#define TX 8

/**
 * @brief Provide the small subset of Arduino String used by s3Monitor.cpp.
 */
class String : public std::string
{
public:
    using std::string::string;
    /**
     * @brief Accept concatenation results from the desktop standard library.
     */
    String(const std::string& value) : std::string(value) {}
    /**
     * @brief Format integer fields as decimal text, as Arduino String does.
     */
    template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
    String(T value) : std::string(std::to_string(value)) {}
};

/**
 * @brief Add Arduino's print API to the shared simulated UART.
 */
class HardwareSerial : public fakeSerial
{
public:
    /**
     * @brief Capture an outgoing command without accessing any physical port.
     */
    size_t print(const String& value)
    {
        output += value;
        return value.size();
    }
};
