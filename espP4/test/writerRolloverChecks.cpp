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
 * @file writerRolloverChecks.cpp
 * Exercise the actual ring past its 32-bit byte-position wrap, without disk I/O.
 * Memory use stays bounded: one ring and one block, not a multi-gigabyte file.
 */
#include "RolloverSink.h"
#include "BufferedWriter.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>

/* **
 * clockUs - Provide deterministic timing; this test measures ordering, not
 * throughput.
 */
static uint64_t clockUs()
{
    return 1;
}

/* **
 * allocate - Use host heap for the external ring, matching the firmware's
 * ownership model.
 */
static uint8_t* allocate(uint32_t bytes)
{
    return static_cast<uint8_t*>(std::malloc(bytes));
}

/* **
 * release - Release the host allocation when the writer goes out of scope.
 */
static void release(uint8_t* memory)
{
    std::free(memory);
}

/* **
 * main - Transfer slightly over 4 GiB, checking actual wrap and 64-bit
 * saved-byte totals.
 */
int main()
{
    BufferedWriter bufferedWriter;
    assert(bufferedWriter.initializeMemory(allocate, release));
    static uint8_t block[BufferedWriter::_blockBytes];
    std::memset(block, 0x5a, sizeof(block));
    RolloverSink rolloverSink;
    rolloverSink._expected = block;
    rolloverSink._expectedBytes = sizeof(block);
    bufferedWriter.start(rolloverSink, clockUs);
    const uint32_t blocks = static_cast<uint32_t>((uint64_t{1} << 32) / sizeof(block)) + 1;
    for (uint32_t iBlock = 0; iBlock < blocks; ++iBlock)
    {
        // Distinguish successive blocks to expose a stale block at wraparound.
        std::memcpy(block, &iBlock, sizeof(iBlock));
        assert(bufferedWriter.submit(block, sizeof(block)));
        assert(bufferedWriter.pump());
    }
    rolloverSink._expectedBytes = 3;
    assert(bufferedWriter.submit(block, 3));
    assert(bufferedWriter.finish());
    const uint64_t expected = (uint64_t{1} << 32) + sizeof(block) + 3;
    const auto statistics = bufferedWriter.getStatistics();
    assert(rolloverSink._total == expected && statistics._bytes == expected);
    assert(statistics._used == 0 && statistics._unsavedBytes == 0 && statistics._errors == 0 && statistics._overflows == 0);
    puts("PASS: actual ring crossed 4 GiB position wrap; byte contents and 64-bit totals match.");
    return 0;
}
