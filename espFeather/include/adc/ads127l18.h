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
 * @file Ads127L18.h
 * @brief Send settings to the ADS127L18 and read its register values back.
 * SPI is the clocked serial link used for these commands. Registers are
 * numbered bytes inside the ADC containing settings, identification, or status.
 * This is not the sample receiver. The separate receiver uses DCLK/FSYNC/DOUT0;
 * replacing that receiver does not change this configuration-port driver. The hardware diagnostic is the first caller of this class.
 */
#pragma once
#include <SPI.h>

/**
 * @brief Read/write ADC registers through slow SPI; construction never starts it.
 * The caller owns the SPI controller and calls begin() only after checking power.
 * Use from one task (Arduino loop in the diagnostic), not from an interrupt or
 * concurrent tasks. No lock is needed because that caller is the only bus user.
 * The ADC must be in SPI mode, with the power-on default SPI CRC disabled.
 * Writes are explicit caller actions; the separate read-only test never uses them.
 */
class Ads127L18
{
public:
    Ads127L18(SPIClass& spiClass, int clockPin, int inputPin, int outputPin, int selectPin);
    void begin();
    bool readRegister(uint8_t address, uint8_t& value);
    bool writeRegister(uint8_t address, uint8_t value);

private:
    SPIClass& _spiClass;
    int _clockPin;
    int _inputPin;
    int _outputPin;
    int _selectPin;
    bool _initialized = false;
    uint8_t transferFrame(uint8_t command, uint8_t data);
};
