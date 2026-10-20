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
 * @file Ads127L18.cpp
 * @brief Read the ADS127L18 registers, allowing for its one-frame reply delay.
 * Reading path: begin() prepares the Feather bus; readRegister() requests a
 * value; transferFrame() performs the actual SPI.transfer() calls. A frame is one
 * group of bytes between taking chip select (CS) low and returning it high.
 * TI ADS127L1x Rev B, sections 7.5.2-7.5.4: mode 1, most-significant bit first,
 * two bytes per frame with CRC disabled. A requested value arrives in the FIRST
 * byte of the NEXT frame, not alongside the request that selected it.
 */
#include "Ads127L18.h"
#include <Arduino.h>

/**
 * @brief Remember the bus and GPIO numbers without touching the hardware.
 * inputPin is Feather MISO (reply), outputPin is MOSI (command), clockPin is
 * SCLK and selectPin is CS. The SPIClass reference must outlive this object.
 */
Ads127L18::Ads127L18(SPIClass& spiClass, int clockPin, int inputPin, int outputPin, int selectPin)
    : _spiClass(spiClass), _clockPin(clockPin), _inputPin(inputPin), _outputPin(outputPin), _selectPin(selectPin)
{

    // Startup is deferred so constructing a global object sends no signals.
}

/**
 * @brief Enable the SPI GPIOs; call only after the board voltage checks pass.
 * CS high means deselected. Passing -1 as the hardware CS pin leaves chip select
 * under our explicit control so every two-byte frame ends at the required point.
 * Returning does not establish ADC presence: SPI has no acknowledgement signal.
 */
void Ads127L18::begin()
{
    if (!_initialized)
    {

        // Preload the output level before enabling it to avoid selecting the ADC
        // unnecessarily while clock/data pins are being assigned to the bus.
        digitalWrite(_selectPin, HIGH);
        pinMode(_selectPin, OUTPUT);
        _spiClass.begin(_clockPin, _inputPin, _outputPin, -1);
        _initialized = true;
    }
}

/**
 * @brief Send one request and return the PREVIOUS request's response byte.
 * The caller has already begun a mode-1 SPI transaction. Command bit 7 is zero
 * for reads or one for writes; data is ignored for reads. transfer()
 * simultaneously sends a byte on MOSI and receives one on MISO. Two-microsecond
 * margins around CS greatly exceed TI's nanosecond timing minima at 100 kHz.
 */
uint8_t Ads127L18::transferFrame(uint8_t command, uint8_t data)
{
    digitalWrite(_selectPin, LOW);
    delayMicroseconds(2);
    const uint8_t retVal = _spiClass.transfer(command);
    _spiClass.transfer(data);
    delayMicroseconds(2);
    digitalWrite(_selectPin, HIGH);
    delayMicroseconds(2);
    return retVal;
}

/**
 * @brief Read any valid register 0x00..0x50 into value without changing it.
 * False means not initialized or unsupported address; value remains unchanged.
 * True means the transfer was performed, NOT that the ADC replied correctly.
 * The diagnostic must check identification and repeatability to judge the link.
 */
bool Ads127L18::readRegister(uint8_t address, uint8_t& value)
{
    bool retVal = false;
    if (_initialized && address <= 0x50)
    {

        // 100,000 clock bits/s is intentionally slow for initial wiring checks.
        // Mode 1 idles clock low and samples on falling clock edges. No other
        // task uses this bus; begin/endTransaction also establish its settings.
        _spiClass.beginTransaction(SPISettings(100000, MSBFIRST, SPI_MODE1));
        transferFrame(address, 0);

        // Repeat the same harmless request to clock out the first reply. This
        // leaves another read pending; the next call discards that stale reply.
        value = transferFrame(address, 0);
        _spiClass.endTransaction();
        retVal = true;
    }
    return retVal;
}

/**
 * @brief Write a writable register and check its echo in the following frame.
 * True means the ADC echoed this write; persistent settings still need readback.
 * CONTROL start/stop bits self-clear, so their echo confirms command acceptance,
 * not that sample clocks reached the receiver. Reject invalid/read-only addresses and
 * changes that enable SPI CRC, which this two-byte transport does not implement.
 */
bool Ads127L18::writeRegister(uint8_t address, uint8_t value)
{
    bool retVal = false;
    const bool writable = address == 2 || (address >= 5 && address <= 0x50);
    if (_initialized && writable && !(address == 0x0a && (value & 0x02)))
    {
        _spiClass.beginTransaction(SPISettings(100000, MSBFIRST, SPI_MODE1));
        transferFrame(static_cast<uint8_t>(0x80 | address), value);

        // A harmless ID read clocks out the previous write's echo. TI inverts
        // that echo for rejected writes; no automatic retry conceals a failure.
        retVal = transferFrame(0, 0) == value;
        _spiClass.endTransaction();
    }
    return retVal;
}
