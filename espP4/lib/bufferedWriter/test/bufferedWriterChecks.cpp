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
 * @file bufferedWriterChecks.cpp
 * @brief Exercise the real byte writer without Arduino, a card, or an ADC.
 * Check ordering through wraps, stalled storage, partial writes, flush failure,
 * byte contents, and concurrent producer/consumer publication on separate threads.
 */
#include <BufferedWriter.h>
#include "bufferedWriterChecks.h"
#include "MemorySink.h"
#include <cassert>
#include <vector>
#include <thread>
#include <iostream>
#include <cstdlib>

uint64_t writerTestUs = 0;

/**
 * @brief Deterministic clock used only by the consumer.
 */
static uint64_t clockUs() { return writerTestUs; }

/**
 * @brief Submit a known four-byte pattern for byte-order checks on the PC.
 * This test helper packs an integer least-significant byte first. It is not
 * an ADC formatter: the writer must copy these bytes without interpreting them.
 */
static bool submitWord(BufferedWriter& bufferedWriter, int32_t value)
{
    const uint32_t bits = static_cast<uint32_t>(value);
    const uint8_t bytes[4] = {static_cast<uint8_t>(bits),
        static_cast<uint8_t>(bits >> 8), static_cast<uint8_t>(bits >> 16),
        static_cast<uint8_t>(bits >> 24)};
    const bool retVal = bufferedWriter.submit(bytes, sizeof(bytes));
    return retVal;
}

/**
 * @brief Run deterministic failures followed by concurrent byte-order stress.
 * The PC runners link the real ring writer to a simulated
 * storage destination. Assertions stop the test if behavior differs from expected.
 * No serial port is opened and nothing is uploaded to either board.
 * First test known byte patterns/timing, then force failures, then exercise a
 * producer thread and a consumer thread concurrently through many ring wraps.
 * These tests check software ordering; they do not measure ESP32 interrupt timing.
 */
int runWriterChecks()
{
    BufferedWriter bufferedWriter;
#if BUFFERED_WRITER_EXTERNAL

    // A failed allocator must not leave a usable-looking writer or null-copy.
    assert(!bufferedWriter.initializeMemory([](uint32_t) -> uint8_t* { return nullptr; },
        [](uint8_t* memory) { std::free(memory); }));
    MemorySink memorySinkMissingMemory;
    bufferedWriter.start(memorySinkMissingMemory, clockUs);
    const uint8_t probe = 1;
    assert(!bufferedWriter.submit(&probe, 1) && bufferedWriter.getFailed());
    bufferedWriter.cancel();
    assert(bufferedWriter.initializeMemory([](uint32_t bytes) -> uint8_t* {
        return static_cast<uint8_t*>(std::malloc(bytes));
    }, [](uint8_t* memory) { std::free(memory); }));
#endif
    MemorySink memorySink;
    bufferedWriter.start(memorySink, clockUs);

    // Send three known four-byte patterns. The generic writer must preserve
    // bytes exactly; it has no knowledge of ADC sample formats.
    submitWord(bufferedWriter, -1);
    submitWord(bufferedWriter, -8388608);
    submitWord(bufferedWriter, 8388607);
    assert(bufferedWriter.finish());
    const std::vector<uint8_t> expected = {255,255,255,255, 0,0,128,255, 255,255,127,0};
    assert(memorySink._bytes == expected);
    assert(bufferedWriter.getStatistics()._maxUs == 45000 && bufferedWriter.getStatistics()._maxKind == 2);
    const auto stoppedTime = bufferedWriter.getStatistics()._elapsedUs;
    writerTestUs += 100000;
    assert(bufferedWriter.getStatistics()._elapsedUs == stoppedTime);

    // A seven-byte submission straddles the physical end of the ring. This
    // exercises both copies in the external-memory path, including odd lengths
    // that four-byte samples alone would never produce. Verify every saved byte.
    memorySink._bytes.clear(); bufferedWriter.start(memorySink, clockUs);
    std::vector<uint8_t> prefix(BufferedWriter::_capacity - 3, 0x5a);
    const uint8_t wrapped[7] = {1, 2, 3, 4, 5, 6, 7};
    assert(bufferedWriter.submit(prefix.data(), static_cast<uint32_t>(prefix.size())));
    assert(bufferedWriter.pump());
    assert(bufferedWriter.submit(wrapped, sizeof(wrapped)));
    assert(bufferedWriter.finish());
    prefix.insert(prefix.end(), wrapped, wrapped + sizeof(wrapped));
    assert(memorySink._bytes == prefix);

    // Force a full buffer while its first block is still in write(). Queued
    // bytes must remain stable; overflow is visible and finish reports failure.
    memorySink._bytes.clear(); bufferedWriter.start(memorySink, clockUs);
    for (int index = 0; index < static_cast<int>(BufferedWriter::_blockBytes / 4); ++index) { submitWord(bufferedWriter, index); }
    memorySink._bufferedWriterDuringWrite = &bufferedWriter;
    bufferedWriter.pump();
    assert(bufferedWriter.getFailed() && bufferedWriter.getStatistics()._overflows == 1);
    assert(!bufferedWriter.finish() && bufferedWriter.getStatistics()._used == 0);

    // Never retry a partially written block. Its accepted prefix is counted,
    // but the queue remains occupied so callers cannot assume everything saved.
    memorySink._bytes.clear(); memorySink._shortWrite = true;
    bufferedWriter.start(memorySink, clockUs); submitWord(bufferedWriter, 42);
    assert(!bufferedWriter.finish());
    assert(memorySink._bytes.size() == 3 && bufferedWriter.getStatistics()._bytes == 3 && bufferedWriter.getStatistics()._used == 4);
    assert(bufferedWriter.getStatistics()._errors == 1);

    // A successful payload write still cannot make finish successful if its
    // final synchronization fails. Then separately test failed preparation timing.
    memorySink._shortWrite = false; memorySink._flushOk = false;
    bufferedWriter.start(memorySink, clockUs);
    assert(!bufferedWriter.finish() && bufferedWriter.getStatistics()._errors == 1);
    bufferedWriter.start(memorySink, clockUs);
    writerTestUs += 123;
    bufferedWriter.measure(writerTestUs - 123, 1, false);
    bufferedWriter.cancel();
    writerTestUs += 100000;
    assert(bufferedWriter.getStatistics()._elapsedUs == 123);

    // A producer pauses BEFORE full capacity (it does not retry rejected data).
    // Real recording stops on full instead; this test isolates publication and
    // ordering through many ring wraps with actual concurrent threads.
    memorySink._flushOk = true; memorySink._bytes.clear(); bufferedWriter.start(memorySink, clockUs);
    std::atomic<uint32_t> consumed{0};
    constexpr uint32_t count = BufferedWriter::_capacity / 4 * 3 + 17;

    // This second OS thread simulates the sample source; it alone calls submit.
    // The main test thread runs pump and publishes how many words were consumed.
    std::thread producer([&]() {

        // Submit increasing values so the consumer can check their order later.
        for (uint32_t index = 0; index < count; ++index)
        {
            while (index - consumed.load(std::memory_order_acquire) >= BufferedWriter::_blockBytes / 4 * 2) { std::this_thread::yield(); }
            assert(submitWord(bufferedWriter, static_cast<int32_t>(index)));
        }
    });
    while (bufferedWriter.getStatistics()._bytes < (count * 4 / BufferedWriter::_blockBytes) * BufferedWriter::_blockBytes)
    {
        assert(bufferedWriter.pump());
        consumed.store(static_cast<uint32_t>(bufferedWriter.getStatistics()._bytes / 4), std::memory_order_release);
        std::this_thread::yield();
    }

    // Wait for the source to finish before draining the final short block.
    // Otherwise finish would race new submissions, violating its contract.
    producer.join();
    assert(bufferedWriter.finish() && memorySink._bytes.size() == count * 4);

    // Decode saved words and check for missing, duplicated or overwritten data
    // across every ring wrap, not just the final reported sample count.
    for (uint32_t index = 0; index < count; ++index)
    {
        const uint8_t* bytes = memorySink._bytes.data() + index * 4;
        const uint32_t value = bytes[0] | (uint32_t(bytes[1]) << 8) |
            (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
        assert(value == index);
    }
    std::cout << "Writer checks passed: bytes, wraps, overflow, partial write, flush failure, concurrent order.\n";

    return 0;
}
