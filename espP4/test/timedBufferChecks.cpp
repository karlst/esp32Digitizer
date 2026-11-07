// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// Run the real ring and boundary adapter against memory-only disk operations.
#include "TimedBufferedSink.h"
#include "MemoryTimedStorage.h"
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <vector>

/* **
 * allocateBytes - Supply owned desktop heap memory for the configurable ring.
 */
static uint8_t* allocateBytes(uint32_t bytes)
{
    return static_cast<uint8_t*>(std::malloc(bytes));
}

/* **
 * releaseBytes - Free the exact pointer returned by the desktop allocator.
 */
static void releaseBytes(uint8_t* bytes)
{
    std::free(bytes);
}

/* **
 * clockUs - Advance a deterministic fake microsecond clock for write statistics.
 */
static uint64_t clockUs()
{
    static uint64_t ticks = 0;
    return ++ticks;
}

/* **
 * checkBoundaries - Queue two differently sized files across chunk boundaries.
 * No storage call is allowed until pump. Then reuse slots enough times to wrap
 * both the byte ring and descriptor queue, checking every retained byte.
 */
static void checkBoundaries()
{
    MemoryTimedStorage memoryTimedStorage;
    TimedBufferedSink timedBufferedSink(memoryTimedStorage);
    assert(timedBufferedSink.initializeMemory(allocateBytes, releaseBytes));
    assert(timedBufferedSink.start(2, clockUs));
    for (uint32_t iFile = 0; iFile < 80; ++iFile)
    {
        const std::vector<uint8_t> expected(333 + iFile % 7, uint8_t(iFile));
        const uint32_t operations = memoryTimedStorage._operations;
        assert(timedBufferedSink.beginFile(iFile % 2));
        assert(timedBufferedSink.submit(expected.data(), 100));
        assert(timedBufferedSink.submit(expected.data() + 100, expected.size() - 100));
        assert(timedBufferedSink.endFile());
        assert(memoryTimedStorage._operations == operations);
        // Two files can share an ordinary 256-byte test chunk. Drain only at
        // every second file; production uses the identical code with 128 KiB.
        assert(timedBufferedSink.pump());
        if (iFile % 2)
        {
            for (unsigned pass = 0; pass < 4; ++pass)
            {
                assert(timedBufferedSink.pump(true));
            }
            assert(memoryTimedStorage._files[iFile % 2] == expected);
        }
    }
    assert(timedBufferedSink.finish());
    assert(memoryTimedStorage._opens == 80 && memoryTimedStorage._closes == 80);
    for (size_t bytes : memoryTimedStorage._writes)
    {
        assert(bytes > 0 && bytes <= BufferedWriter::_blockBytes);
    }
    const auto stats = timedBufferedSink.getStatistics();
    assert(stats._errors == 0 && stats._overflows == 0 && stats._unsavedBytes == 0);
}

/* **
 * checkPublishedEnd - Permit a final boundary to arrive after its bytes were
 * written. The next pump must close it without another byte or duplicate write.
 */
static void checkPublishedEnd()
{
    MemoryTimedStorage memoryTimedStorage;
    TimedBufferedSink timedBufferedSink(memoryTimedStorage);
    assert(timedBufferedSink.initializeMemory(allocateBytes, releaseBytes));
    assert(timedBufferedSink.start(1, clockUs));
    std::vector<uint8_t> data(256, 7);
    assert(timedBufferedSink.beginFile(0));
    assert(timedBufferedSink.submit(data.data(), data.size()));
    assert(timedBufferedSink.pump());
    assert(memoryTimedStorage._closes == 0);
    assert(timedBufferedSink.endFile());
    assert(timedBufferedSink.pump());
    assert(memoryTimedStorage._closes == 1 && memoryTimedStorage._writes.size() == 1);
    assert(timedBufferedSink.finish());
}

/* **
 * checkFailures - Inject open, partial write, close, incomplete-file and queue
 * failures. Failed writes must not be retried during finish or reported clean.
 */
static void checkFailures()
{
    for (unsigned failure = 0; failure < 4; ++failure)
    {
        MemoryTimedStorage memoryTimedStorage;
        memoryTimedStorage._failOpen = failure == 0;
        memoryTimedStorage._shortWrite = failure == 1;
        memoryTimedStorage._failClose = failure == 2;
        TimedBufferedSink timedBufferedSink(memoryTimedStorage);
        assert(timedBufferedSink.initializeMemory(allocateBytes, releaseBytes));
        assert(timedBufferedSink.start(1, clockUs));
        const uint8_t data[256] = {};
        assert(timedBufferedSink.beginFile(0));
        assert(timedBufferedSink.submit(data, sizeof(data)));
        if (failure != 3)
        {
            assert(timedBufferedSink.endFile());
        }
        assert(!timedBufferedSink.finish());
        assert(timedBufferedSink.getFailed());
        assert(memoryTimedStorage._writes.size() <= 1);
    }
    MemoryTimedStorage memoryTimedStorage;
    TimedBufferedSink timedBufferedSink(memoryTimedStorage);
    assert(timedBufferedSink.initializeMemory(allocateBytes, releaseBytes));
    assert(timedBufferedSink.start(1, clockUs));
    const uint8_t byte = 0;
    for (unsigned iFile = 0; iFile < 32; ++iFile)
    {
        assert(timedBufferedSink.beginFile(0));
        assert(timedBufferedSink.submit(&byte, 1));
        assert(timedBufferedSink.endFile());
    }
    assert(!timedBufferedSink.beginFile(0));
    assert(timedBufferedSink.getFailed() && memoryTimedStorage._operations == 0);
    assert(!timedBufferedSink.finish());
}

/* **
 * main - Validate the reusable boundary adapter without building board firmware.
 */
int main()
{
    static_assert(BufferedWriter::_capacity == 1024 && BufferedWriter::_blockBytes == 256,
        "This test intentionally exercises a small ring with many wraps");
    checkBoundaries();
    checkPublishedEnd();
    checkFailures();
    std::puts("PASS: buffered file boundaries, no submission I/O, wraps and disk failures");
    return 0;
}
