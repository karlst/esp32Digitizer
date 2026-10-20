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

/** @file sdTestData.h
 * @brief Hardware-independent timing and data pattern for the SD-only test.
 */
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "recordingFormat.h"

namespace sdTestData
{
constexpr uint64_t durationUs = 60000000;
constexpr uint32_t stage = 0xffffffffU;

/** @brief Number of four-byte words due by elapsed microseconds, capped at 60 s.
 * Integer arithmetic avoids accumulated rounding from individual task sleeps.
 * The two selected rates are 812500 and 1024000 words/s (26 and 32.768 Mbps).
 */
inline uint64_t dueWords(uint64_t elapsedUs, uint32_t wordsPerSecond)
{
    const uint64_t limited = elapsedUs < durationUs ? elapsedUs : durationUs;
    return limited * wordsPerSecond / 1000000;
}

/** @brief Deterministic full-width word; file readback independently checks each index. */
inline uint32_t word(uint32_t index)
{
    return (index * 1664525U + 1013904223U) ^ 0xa57c391eU;
}
/**
 * @brief Check card-read bytes against the generated test pattern; return mismatches.
 * firstWord is the absolute four-byte word index at the start of this block.
 * The recorder supplies bytes read from SD, not the original generator buffer.
 * An incomplete final word counts as one error. This does not perform card I/O.
 */
inline uint64_t checkBlock(const uint8_t* data, size_t bytes, uint64_t firstWord)
{
    uint64_t retVal = 0;
    for (size_t offset = 0; offset + 4 <= bytes; offset += 4)
    {
        uint32_t observed = 0;
        std::memcpy(&observed, data + offset, sizeof(observed));
        if (observed != word(static_cast<uint32_t>(firstWord + offset / 4)))
        {
            ++retVal;
        }
    }
    if (bytes % 4 != 0) { ++retVal; }
    return retVal;
}
/**
 * @brief Write the original 512-byte synthetic-test header into caller-owned RAM.
 * Byte count excludes the header; these 60-second tests fit in 32 bits. valid is
 * the caller's verdict, not a check performed here. Integer encoders are shared
 * with the real ADC file format; the payload layout and magic remain test-only.
 */
inline void makeHeader(uint8_t* output, bool valid, uint32_t run, uint32_t stageId,
                       uint32_t rate, uint64_t bytes, uint32_t crc)
{
    std::memset(output, 0, recordingFormat::headerBytes);
    std::memcpy(output, "P4SYN01", 7);
    recordingFormat::store32(output + 8, 1);
    recordingFormat::store32(output + 12, valid ? 1 : 0);
    recordingFormat::store32(output + 16, run);
    recordingFormat::store32(output + 20, stageId);
    recordingFormat::store32(output + 24, rate);
    recordingFormat::store32(output + 28, static_cast<uint32_t>(bytes));
    recordingFormat::store32(output + 32, crc);
}
}
