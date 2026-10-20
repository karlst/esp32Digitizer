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
 * @file SPI.h
 * @brief Define a pretend SPI controller for desktop tests.
 * ads127DriverChecks.cpp implements these methods and interprets commands in
 * memory. The executable never accesses SPI pins. API type names match Arduino.
 */
#pragma once
#include <cstdint>
#include <cstddef>
#define FSPI 0
#define HSPI 1
#define MSBFIRST 1
#define SPI_MODE1 1

/**
 * @brief Retain the driver's chosen clock and mode for assertions.
 */
struct SPISettings
{
    uint32_t _frequency;
    int _order;
    int _mode;

    /**
     * @brief Store requested bus settings.
     */
    SPISettings(uint32_t frequency, int order, int mode) : _frequency(frequency), _order(order), _mode(mode) {}
};

/**
 * @brief Provide the driver member's type without opening hardware.
 */
class SPIClass
{
public:
    /**
     * @brief Retain construction compatibility only.
     */
    explicit SPIClass(int) {}
    void begin(int clock, int input, int output, int chipSelect);
    void beginTransaction(SPISettings settings);
    void endTransaction();
    uint8_t transfer(uint8_t byte);
    void transferBytes(const uint8_t* input, uint8_t* output, uint32_t count);
    void writeBytes(const uint8_t* bytes, uint32_t count);
};
