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
 * @file ads127InputDiagnostic.cpp
 * @brief Park S3 sample pins as inputs during ADS127L18 power and SPI register checks.
 * This standalone build excludes the old ADS1256 driver, recording service and
 * command handlers. Therefore a web Start command cannot start old hardware.
 * There is no DMA, interrupt handler, SD access or waveform measurement here.
 */
#include <Arduino.h>
#include "ads127Pins.h"

/**
 * @brief Report the intended input configuration, not a hardware self-test.
 */
static void printHelp()
{
    Serial.println("ADS127 INPUT-ONLY: DCLK GPIO9, FSYNC GPIO10, DOUT0 GPIO11 are inputs without pulls.");
    Serial.println("Acquisition, DMA, SD writes and Feather command handling are absent in this build.");
    Serial.println("This is a commissioning build, not a capture-speed test. Send h to repeat.");
}

/**
 * @brief Release the three sample pins before opening the diagnostic terminal.
 * pinMode(INPUT) disables output drive and pull resistors. Nothing later changes
 * these pins. This cannot control pin behavior before application startup, so
 * disable translator outputs during upload and restore connections only with power off.
 */
void setup()
{
    pinMode(ads127Pins::s3DataClock, INPUT);
    pinMode(ads127Pins::s3FrameSync, INPUT);
    pinMode(ads127Pins::s3Data, INPUT);
    Serial.begin(115200);
    delay(500);
    printHelp();
}

/**
 * @brief Keep the board idle; USB help is the only recognized command.
 * delay(1) yields the Arduino task so it does not spin continuously.
 */
void loop()
{
    if (Serial.available())
    {
        const char command = static_cast<char>(Serial.read());
        if (command == 'h' || command == 'H')
        {
            printHelp();
        }
    }
    delay(1);
}
