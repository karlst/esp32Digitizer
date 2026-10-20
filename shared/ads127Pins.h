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
 * @file ads127Pins.h
 * @brief GPIO assignments for the ADS127L18 wiring agreed on October 10, 2026.
 * Numbers are ESP32 GPIO numbers, not physical connector positions. Feather
 * means the ESP32 V2 board; S3 means the separate recorder. All seven digital
 * signals pass through the SN74AXC8T245 translator, A=3.3 V and B=1.8 V.
 * This header only names wires; including it does not configure hardware.
 * The s3-prefixed inputs are legacy assignments. P4 uses GPIO0/1/2 instead,
 * configured in espP4/main/src/main.cpp; do not use the S3 numbers for P4.
 */
#pragma once

namespace ads127Pins
{

    // Feather commands travel A->B; the reply travels B->A on channel 8.
    constexpr int featherClock = 5;      // SCK -> A2/B2 -> ADC SCLK.
    constexpr int featherMosi = 19;      // MO  -> A3/B3 -> ADC SDI.
    constexpr int featherSelect = 33;    // GPIO33 -> A4/B4 -> ADC CS.
    constexpr int featherMiso = 21;      // MI <- A8/B8 <- ADC SDO.

    // ADC sample signals travel B->A. They must never be GPIO outputs on S3.
    constexpr int s3DataClock = 9;       // DCLK -> B5/A5 -> S3.
    constexpr int s3FrameSync = 10;      // FSYNC -> B6/A6 -> S3.
    constexpr int s3Data = 11;           // DOUT0 -> B7/A7 -> S3.
}
