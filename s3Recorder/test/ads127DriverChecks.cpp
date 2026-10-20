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
 * @file ads127DriverChecks.cpp
 * @brief Exercise the real Feather ADS127 driver against a delayed-reply SPI fake.
 * Run through runNativeChecks.ps1 on the PC; no board or serial port is used.
 * The fake begins with a stale reply and changes register values between reads.
 * This catches returning the request frame's old byte instead of the next frame.
 */
#include <cassert>
#include <cstdio>
#include "Arduino.h"
#include "SPI.h"
#include "ads127l18.h"
#include "ads127LowRate.h"
#include "test/support/benchLine.h"
#include "ads127BenchRate.h"

// Existing Arduino fakes use these globals to observe pin levels and elapsed time.
uint32_t testMs = 0, testUs = 0;
int testReady = 0, testChipSelect = HIGH;
static unsigned beginCount = 0, transferCount = 0, transactionBytes = 0;
static bool transactionOpen = false;
fakeSerial Serial;
static uint8_t registers[0x51] = {0x06, 0x23, 0x60, 0x00, 0x00, 0x55, 0xaa};
static uint8_t pendingReply = 0xa5, requestedAddress = 0;
static bool writeFrame = false, rejectWrite = false;
static unsigned startCommands = 0, stopCommands = 0;
static unsigned statusWrites = 0;
static uint8_t stickyStatus = 0;
static bool rejectStatusWrite = false;

/**
 * @brief Check that begin selects the agreed Feather GPIOs and manual CS.
 */
void SPIClass::begin(int clock, int input, int output, int chipSelect)
{
    assert(clock == 5 && input == 21 && output == 19 && chipSelect == -1);
    assert(testChipSelect == HIGH);
    ++beginCount;
}

/**
 * @brief Check the diagnostic's slow clock and TI-required SPI mode.
 */
void SPIClass::beginTransaction(SPISettings settings)
{
    assert(!transactionOpen && beginCount == 1);
    assert(settings.frequency == 100000 && settings.order == MSBFIRST && settings.mode == SPI_MODE1);
    transactionOpen = true;
    transactionBytes = 0;
}

/**
 * @brief Return the previous frame's value while accepting the next read request.
 * The second response byte deliberately differs from all useful register values.
 * Writes return an echo in the next frame; CONTROL bits self-clear in readback.
 */
uint8_t SPIClass::transfer(uint8_t byte)
{
    assert(transactionOpen && testChipSelect == LOW);
    uint8_t retVal = 0xee;
    if (transactionBytes % 2 == 0)
    {
        requestedAddress = byte & 0x7f;
        writeFrame = (byte & 0x80) != 0;
        assert(requestedAddress <= 0x50);
        retVal = pendingReply;
    }
    else
    {
        if (writeFrame)
        {
            const bool rejected = rejectWrite || (requestedAddress == 2 && rejectStatusWrite);
            pendingReply = rejected ? static_cast<uint8_t>(~byte) : byte;
            if (!rejected)
            {
                if (requestedAddress == 2)
                {

                    // STATUS is write-one-to-clear, unlike configuration RAM.
                    // Sticky bits simulate a fault remaining after acknowledgment.
                    assert(byte == 0x60);
                    ++statusWrites;
                    registers[2] = (registers[2] & ~byte) | stickyStatus;
                }
                else if (requestedAddress == 7)
                {
                    startCommands += byte == 2;
                    stopCommands += byte == 1;
                    registers[7] = 0;
                }
                else
                {
                    registers[requestedAddress] = byte;
                }
            }
        }
        else
        {
            assert(byte == 0);
            pendingReply = registers[requestedAddress];
        }
    }
    ++transactionBytes;
    ++transferCount;
    return retVal;
}

/**
 * @brief Require exactly two complete two-byte frames and a deselected ADC.
 */
void SPIClass::endTransaction()
{
    assert(transactionOpen && transactionBytes == 4 && testChipSelect == HIGH);
    transactionOpen = false;
}

/**
 * @brief Check reply delay, configuration, write failures and absent-device data.
 * The driver reports transfer completion separately from identity validation;
 * zero/FF replies must remain visible to the diagnostic rather than become success.
 */
int main()
{
    SPIClass bus(0);
    ads127l18 adc(bus, 5, 21, 19, 33);
    uint8_t value = 0xcc;
    assert(beginCount == 0 && transferCount == 0);
    assert(!adc.readRegister(0, value) && value == 0xcc);
    adc.begin();
    adc.begin();
    assert(beginCount == 1);

    // Different requested registers must never return the preceding one's data.
    for (uint8_t address = 0; address <= 6; ++address)
    {
        assert(adc.readRegister(address, value) && value == registers[address]);
    }
    registers[1] = 0x42;
    assert(adc.readRegister(1, value) && value == 0x42);

    // Reject addresses above the valid map without I/O; configuration is readable.
    const unsigned beforeRejected = transferCount;
    for (unsigned address = 0x51; address <= 255; ++address)
    {
        assert(!adc.readRegister(static_cast<uint8_t>(address), value) && value == 0x42);
    }
    assert(transferCount == beforeRejected);

    // No writes to ID/read-only registers or enabling unsupported CRC framing.
    assert(!adc.writeRegister(0, 0x06));
    assert(!adc.writeRegister(3, 0));
    assert(!adc.writeRegister(0x51, 0));
    assert(!adc.writeRegister(0x0a, 0x82));
    assert(transferCount == beforeRejected);
    assert(adc.writeRegister(0x0c, 0x60));
    assert(adc.readRegister(0x0c, value) && value == 0x60);
    rejectWrite = true;
    assert(!adc.writeRegister(0x0c, 0x20));
    assert(registers[0x0c] == 0x60);
    rejectWrite = false;

    // Configuration enables only channel zero, uses internal input and never starts.
    ads127LowRate settings(adc);
    assert(!settings.start());
    assert(settings.configure());
    assert(startCommands == 0 && stopCommands == 2);
    assert(statusWrites == 1 && registers[2] == 0);
    assert(registers[9] == 6 && registers[0x0c] == 0x60);

    // TI CLK_CFG bit 3 is the external-clock selector. Zero silently chooses
    // the internal 25.6-MHz oscillator, invalidating the intended 16-kHz rate.
    assert(registers[0x0d] == 0x08);
    assert(registers[0x0b] == 0x70 && registers[0x11] == 0x20);
    for (unsigned channel = 0; channel < 8; ++channel)
    {
        assert(registers[0x12 + channel * 8] == (channel == 0 ? 0x10 : 0x30));
    }
    assert(settings.start() && startCommands == 1);
    assert(settings.stop() && stopCommands == 3);
    rejectWrite = true;
    assert(!settings.configure() && !settings.start());
    rejectWrite = false;

    // Clearing startup history must not conceal other faults or start acquisition.
    const unsigned priorStarts = startCommands;
    for (uint8_t fault = 1; fault <= 0x10; fault <<= 1)
    {
        registers[2] = 0x60 | fault;
        const unsigned priorClears = statusWrites;
        assert(!settings.configure() && !settings.start());
        assert(registers[2] == (0x60 | fault) && statusWrites == priorClears);
    }
    registers[2] = 0x60;
    rejectStatusWrite = true;
    assert(!settings.configure() && !settings.start() && registers[2] == 0x60);
    rejectStatusWrite = false;
    stickyStatus = 0x40;
    assert(!settings.configure() && !settings.start() && registers[2] == 0x40);
    stickyStatus = 0;
    registers[2] = 0;
    const unsigned priorClears = statusWrites;
    assert(settings.configure() && settings.reportStatus("clean test"));
    assert(statusWrites == priorClears);

    // A fault appearing after configuration is reported without being erased.
    registers[2] = 0x04;
    assert(!settings.reportStatus("after STOP") && !settings.start());
    assert(registers[2] == 0x04 && statusWrites == priorClears);
    assert(startCommands == priorStarts);

    // Check every timing stage against clock arithmetic, not just matching bytes.
    registers[2] = 0;
    for (unsigned stage = 0; stage < ads127BenchRateCount; ++stage)
    {
        const ads127BenchRate& rate = ads127BenchRates[stage];
        assert(rate.samplesPerSecond == 16384000u / rate.osr);
        const unsigned divider = 1u << (rate.clockDividerCode >> 5);
        assert(32768000u / divider == rate.samplesPerSecond * rate.slots * 32);
        assert(settings.configure(stage));

        // Every stage keeps 24-bit output and full drive strength. This checks
        // the configuration command, not physical signal quality on the wires.
        assert(registers[0x0a] == 0x00);
        assert(registers[0x0c] == rate.clockDividerCode);
        for (unsigned channel = 0; channel < 8; ++channel)
        {
            assert(registers[0x12 + 8 * channel] == (rate.filterCode | (channel ? 0x20 : 0)));
        }
    }
    const unsigned beforeInvalidStage = transferCount;
    assert(!settings.configure(ads127BenchRateCount) && !settings.start());
    assert(transferCount == beforeInvalidStage);

    // Oversized UART lines must be dropped, never interpreted as a valid suffix.
    benchLine line;
    for (unsigned index = 0; index < 400; ++index)
    {
        assert(!line.feed('x'));
    }
    assert(!line.feed('\n'));
    assert(!line.feed('h') && line.feed('\n'));
    assert(line.value()[0] == 'h' && line.value()[1] == '\0');

    // A disconnected bus can return either extreme; the driver must not invent ID.
    registers[0] = 0;
    assert(adc.readRegister(0, value) && value == 0);
    registers[0] = 0xff;
    assert(adc.readRegister(0, value) && value == 0xff);
    assert(!transactionOpen && testChipSelect == HIGH);
    assert(!settings.configure() && !settings.start());
    std::puts("ADS127 checks passed: delayed replies/echoes, startup-only flag clearing, retained/recurrent faults, start refusal, bad ID, UART bounds.");
    const int retVal = 0;
    return retVal;
}
