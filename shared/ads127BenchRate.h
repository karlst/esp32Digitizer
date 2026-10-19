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
 * @file ads127BenchRate.h
 * @brief Shared ADC/receiver timing for capture qualification through 1.024 Msample/s.
 * The EVM clock is 32.768 MHz and the modulator uses half that frequency.
 * OSR is how many modulator readings the filter combines per output sample.
 * A slot is 32 clocks on DOUT0; only the first carries our status+sample word.
 * Both boards use the same stage number so their timing cannot drift apart.
 */
#pragma once
#include <cstdint>

/** @brief ADC clock/OSR choices; S3 reception passed through 256k and failed above it. */
struct ads127BenchRate
{
    uint32_t samplesPerSecond;
    uint16_t osr;
    uint8_t filterCode;
    uint8_t clockDividerCode;
    uint8_t slots;
};

static constexpr ads127BenchRate ads127BenchRates[] = {
    {16000, 1024, 0x10, 0x60, 8},
    {32000, 512, 0x0f, 0x60, 4},
    {64000, 256, 0x0e, 0x60, 2},
    {128000, 128, 0x0d, 0x40, 2},
    {256000, 64, 0x0c, 0x20, 2},
    {512000, 32, 0x0b, 0x00, 2},
    {1024000, 16, 0x09, 0x00, 1}
};
static constexpr unsigned ads127BenchRateCount = sizeof(ads127BenchRates) / sizeof(ads127BenchRates[0]);
