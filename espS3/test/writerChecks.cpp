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
 * @file writerChecks.cpp
 * @brief Check S3 sample formatting and card-space accounting on the PC.
 * Generic ring and failure checks live in shared/bufferedWriter/test and are
 * called first. These additional assertions cover the S3 four-byte file format.
 */
#include "sampleFormatter.h"
#include "recordingSpace.h"
#include "memorySink.h"
#include "bufferedWriterChecks.h"
#include <cstdlib>
#include <iostream>

/** @brief Return the shared test clock; no real-time or board access occurs. */
static uint64_t clockUs() { return writerTestUs; }

/**
 * @brief Run common writer checks, then S3-specific format and allocation checks.
 * Assertions stop the executable on failure. This does not test a physical card.
 */
int main()
{
    runWriterChecks();
    bufferedWriter writer;
#if BUFFERED_WRITER_EXTERNAL

    // A failed allocator must not leave a usable-looking writer or null-copy.
    assert(!writer.initializeMemory([](uint32_t) -> uint8_t* { return nullptr; },
        [](uint8_t* memory) { std::free(memory); }));
    memorySink missingMemorySink;
    writer.start(missingMemorySink, clockUs);
    const uint8_t probe = 1;
    assert(!writer.submit(&probe, 1) && writer.failed());
    writer.cancel();
    assert(writer.initializeMemory([](uint32_t bytes) -> uint8_t* {
        return static_cast<uint8_t*>(std::malloc(bytes));
    }, [](uint8_t* memory) { std::free(memory); }));
#endif
    memorySink sink;
    writer.start(sink, clockUs);

    // Use negative one and both signed 24-bit extremes to catch wrong byte
    // order or missing sign extension. Expected bytes are written out explicitly.
    sampleFormatter::submit(writer, -1);
    sampleFormatter::submit(writer, -8388608);
    sampleFormatter::submit(writer, 8388607);
    assert(writer.finish());
    const std::vector<uint8_t> expected = {255,255,255,255, 0,0,128,255, 255,255,127,0};
    assert(sink.bytes == expected);
    assert(writer.snapshot().maxUs == 45000 && writer.snapshot().maxKind == 2);
    const auto stoppedTime = writer.snapshot().elapsedUs;
    writerTestUs += 100000;
    assert(writer.snapshot().elapsedUs == stoppedTime);

    // Initial scan already includes the first 512-byte header's full cluster.
    // Rewriting that header cannot consume more allocation. New parts round
    // independently, and a full card clamps to zero rather than unsigned wrap.
    recordingSpace space;
    const uint64_t firstHeader = recordingSpace::allocation(512, 4096);
    assert(firstHeader == 4096 && recordingSpace::allocation(0, 4096) == 0);
    assert(recordingSpace::allocation(4096, 4096) == 4096);
    assert(recordingSpace::allocation(4097, 4096) == 8192);
    assert(recordingSpace::allocation(512, 0) == 0);
    space.begin(10000, firstHeader);
    assert(space.remaining(firstHeader) == 10000);
    assert(space.remaining(recordingSpace::allocation(4097, 4096)) == 5904);
    assert(space.remaining(recordingSpace::allocation(4097, 4096) + firstHeader) == 1808);
    assert(space.remaining(65536) == 0);
    space.begin(80000000000ULL, 4096);
    assert(space.remaining(8192) == 79999995904ULL);
    std::cout << "S3 format and recording-space checks passed.\n";
    return 0;
}
