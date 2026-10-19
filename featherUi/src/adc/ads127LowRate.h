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
 * @file ads127LowRate.h
 * @brief Selectable ADC bench configurations, independent of capture success.
 */
#pragma once
#include "ads127l18.h"

/**
 * @brief Configure channel zero's selected bench input and control conversions.
 * Called only by the Feather diagnostic's Arduino task. The ADC driver owns
 * SPI transfers; this class owns their sequence and checks configuration readback.
 * No output starts at construction or configuration time. A separate start()
 * follows the S3 READY reply so the receiver exists before samples are produced.
 */
class ads127LowRate
{
public:
    explicit ads127LowRate(ads127l18& adc);
    bool configure(unsigned stage = 0, bool externalInput = false);
    bool start();
    bool stop();
    bool reportStatus(const char* phase);
private:
    ads127l18& adc;
    bool configured = false;
    bool setAndCheck(uint8_t address, uint8_t value);
    bool readStatus(const char* phase, uint8_t& status);
    bool clearStartupFlags();
};
