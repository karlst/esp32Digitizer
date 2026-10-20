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

/** @file main.cpp
 * @brief Arduino entry points for the ordinary external-input ADC controller.
 * Objects have static lifetime because callbacks and loop continue after setup.
 * No DAC test generator or A2 observation buffer is linked into this program.
 */
#include "AcquisitionApplication.h"
#include "AcquisitionServices.h"
#include "ads127Pins.h"
static Ads127L18 ads127L18(SPI, ads127Pins::featherClock, ads127Pins::featherMiso,
                     ads127Pins::featherMosi, ads127Pins::featherSelect);
static Ads127LowRate ads127LowRate(ads127L18);
static AcquisitionServices acquisitionServices;
static AcquisitionApplication acquisitionApplication(ads127LowRate, acquisitionServices,
    1800000, 1805000);
/** @brief Arduino calls once; initialize stopped hardware and command ports. */
void setup() { acquisitionApplication.initialize(); }
/** @brief Arduino calls repeatedly; service commands, replies and capture deadlines. */
void loop() { acquisitionApplication.poll(); }
