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

/** @file recordingFormat.h
 * @brief Define how ADC numbers and recording information are stored in P4 ADC files.
 * ADC v3 stores three-byte signed samples and 64-bit sizes.
 * These helpers do not allocate memory, perform disk I/O, or change DMA capture.
 * This is the SECOND kind of packing in the data path. PARLIO hardware first
 * packs pairs of pin levels into raw bytes. frameDecoder reconstructs 32-bit
 * words. packSamples() here then removes the status byte and arranges the three
 * sample bytes for the file. These operations must not be confused: hardware
 * sees pin levels; this software sees already-decoded ADC values.
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace recordingFormat
{
constexpr size_t headerBytes = 512;
constexpr size_t sampleBytes = 3;

/** @brief Copy the low 24 bits of each decoded ADC word, least-significant byte first.
 * The decoder has already checked the high status byte; it is not saved here.
 * For example 0x08ffffff becomes FF FF FF (-1), and 0x08800000 becomes
 * 00 00 80 (-8388608). Readers sign-extend bit 23 when reconstructing a sample.
 * Capacity is bytes; return copied bytes, or zero without copying if too small.
 */
inline size_t packSamples(const uint32_t* words, size_t count, uint8_t* output, size_t capacity)
{
    size_t retVal = 0;
    if (count <= capacity / sampleBytes)
    {
        for (size_t index = 0; index < count; ++index)
        {
            const uint32_t word = words[index];
            output[index * 3] = static_cast<uint8_t>(word);
            output[index * 3 + 1] = static_cast<uint8_t>(word >> 8);
            output[index * 3 + 2] = static_cast<uint8_t>(word >> 16);
        }
        retVal = count * sampleBytes;
    }
    return retVal;
}

/**
 * @brief Store value in four bytes at output, least-significant byte first.
 * The caller must provide four writable bytes; no capacity check occurs here.
 */
inline void store32(uint8_t* output, uint32_t value)
{
    for (unsigned index = 0; index < 4; ++index)
    {
        output[index] = static_cast<uint8_t>(value >> (index * 8));
    }
}

/**
 * @brief Store value in eight writable bytes supplied by the caller.
 * Low four bytes come first. This retains file sizes larger than 4 GiB.
 */
inline void store64(uint8_t* output, uint64_t value)
{
    store32(output, static_cast<uint32_t>(value));
    store32(output + 4, static_cast<uint32_t>(value >> 32));
}

/** @brief Fill caller-owned 512-byte header; version identifies the payload layout.
 * ADC v3: magic P4ADC03, 64-bit byte count at 28, CRC32 at 36, width=3 at 40,
 * and 64-bit sample count at 44. Readers must select layout by version/magic.
 * run identifies the capture; rate is samples per second. Bytes 20-23 are
 * reserved and written as zero; older files stored a configuration index there.
 * bytes excludes this header. Output must have
 * 512 writable bytes. The helper simply encodes the caller's valid flag; it
 * performs no verification itself. The recorder supplies true after capture
 * and readback checks, but final sync/close occur afterward. Payload bytes
 * and CRC describe the stored representation, not the larger DMA input stream.
 */
inline void makeHeader(uint8_t* output, bool valid, uint32_t run,
                       uint32_t rate, uint64_t bytes, uint32_t crc)
{
    std::memset(output, 0, headerBytes);
    std::memcpy(output, "P4ADC03", 7);
    store32(output + 8, 3);
    store32(output + 12, valid ? 1 : 0);
    store32(output + 16, run);
    store32(output + 20, 0); // Reserved; retain all existing field offsets.
    store32(output + 24, rate);
    store64(output + 28, bytes);
    store32(output + 36, crc);
    store32(output + 40, sampleBytes);
    store64(output + 44, bytes / sampleBytes);
}
}
