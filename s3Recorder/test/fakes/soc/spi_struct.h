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
 * @file spi_struct.h
 * @brief Desktop model of only the S3 SPI registers used by fastCapture.
 */
#pragma once
#include <cstdint>

/** @brief Register storage advanced by a test clock hook, not physical hardware. */
struct fakeSpiRegisters
{
    struct { uint32_t usr = 0; uint32_t update = 0; } cmd;
    struct { uint32_t ms_data_bitlen = 0; } ms_dlen;
    uint32_t data_buf[16] = {};
};
inline fakeSpiRegisters GPSPI2;
