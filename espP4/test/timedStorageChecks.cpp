// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// Full format -> byte ring -> FatFs adapter checks on an in-memory filesystem.
#include "TimedRecordingStream.h"
#include "TimedBufferedSink.h"
#include "TimedFatStorage.h"
#include "MemoryRecordingDisk.h"
#include "MemoryTimedSink.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>

/* **
 * allocateBytes - Supply desktop memory; hardware startup supplies internal RAM
 * for scratch and PSRAM for the ring using separate allocator functions.
 */
static uint8_t* allocateBytes(uint32_t bytes)
{
    return static_cast<uint8_t*>(std::malloc(bytes));
}

/* **
 * releaseBytes - Release a desktop allocation after all simulated work stops.
 */
static void releaseBytes(uint8_t* bytes)
{
    std::free(bytes);
}

/* **
 * clockUs - Deterministic elapsed-time source; not a disk-speed measurement.
 */
static uint64_t clockUs()
{
    static uint64_t time = 0;
    return ++time;
}

/* **
 * checkReadback - Compare every retained byte with direct formatter output.
 * Slot/offset mapping after circular reuse must agree, including headers/markers.
 */
static bool checkReadback(void* context, uint32_t iSlot, uint64_t offset,
    const uint8_t* data, uint32_t bytes)
{
    const auto& expected = static_cast<MemoryTimedSink*>(context)->_files.at(iSlot);
    const bool retVal = offset <= expected.size() && bytes <= expected.size() - offset &&
        std::memcmp(expected.data() + offset, data, bytes) == 0;
    return retVal;
}

/* **
 * checkStorage - Prepare two new files without formatting, then reuse them.
 * Compare buffered FatFs contents with direct formatter output; old user files
 * must remain unchanged, and header bytes must never be patched at close.
 */
static void checkStorage()
{
    MemoryRecordingDisk memoryRecordingDisk;
    MemoryRecordingDisk::files()["0:/important.txt"] = {1, 2, 3, 4};
    TimedFatStorage timedFatStorage;
    assert(timedFatStorage.initializeMemory(allocateBytes, releaseBytes));
    assert(timedFatStorage.prepare(memoryRecordingDisk, 2, 4096));
    TimedBufferedSink timedBufferedSink(timedFatStorage);
    assert(timedBufferedSink.initializeMemory(allocateBytes, releaseBytes));
    assert(timedBufferedSink.start(2, clockUs));
    TimedRecordingConfig config;
    config._fileCount = 2;
    config._pairsPerFile = 1;
    MemoryTimedSink memoryTimedSink;
    TimedRecordingStream expectedStream(memoryTimedSink);
    TimedRecordingStream actualStream(timedBufferedSink);
    assert(expectedStream.start(1000, config) && actualStream.start(1000, config));
    MemoryRecordingDisk::clearWrites();
    for (uint32_t iInterval = 0; iInterval < 5; ++iInterval)
    {
        TimeBlock timeBlock;
        timeBlock._tcPpsFirst = uint64_t(iInterval) * 160000000;
        timeBlock._tcPpsLast = timeBlock._tcPpsFirst + 160000000;
        timeBlock._tcFsFirst = timeBlock._tcPpsFirst + 100;
        timeBlock._tcFsLast = timeBlock._tcPpsFirst + 256;
        timeBlock._sCount = 2;
        const uint8_t samples[] = {uint8_t(iInterval), 0, 0, 0xff, 0xff, 0xff};
        assert(expectedStream.beginBlock(timeBlock) && actualStream.beginBlock(timeBlock));
        assert(expectedStream.submitSamples(samples, 2) && actualStream.submitSamples(samples, 2));
        // Disk calls begin only here, not in either formatter invocation above.
        assert(timedBufferedSink.pump(true));
        assert(timedBufferedSink.pump(true));
    }
    assert(actualStream.finish() && expectedStream.finish());
    assert(timedBufferedSink.finish());
    assert(timedFatStorage.readback(checkReadback, &memoryTimedSink));
    assert(MemoryRecordingDisk::files().at("0:/important.txt") == std::vector<uint8_t>({1, 2, 3, 4}));
    for (uint32_t iSlot = 0; iSlot < 2; ++iSlot)
    {
        char name[20];
        std::snprintf(name, sizeof(name), "/F%04lu.DG", (unsigned long)iSlot);
        const auto& file = MemoryRecordingDisk::files().at(std::string(timedFatStorage.getDirectory()) + name);
        assert(file.size() == 4096);
        assert(std::memcmp(file.data(), memoryTimedSink._files[iSlot].data(), memoryTimedSink._files[iSlot].size()) == 0);
    }

    // Preparation creates another directory, leaving the completed session intact.
    const auto saved = MemoryRecordingDisk::files();
    assert(timedFatStorage.prepare(memoryRecordingDisk, 1, 4096));
    for (const auto& entry : saved)
    {
        assert(MemoryRecordingDisk::files().at(entry.first) == entry.second);
    }
    assert(timedFatStorage.openSlot(0));
    const uint8_t bytes[20] = {};
    MemoryRecordingDisk::failWrite(true);
    assert(timedFatStorage.write(bytes, sizeof(bytes)) < sizeof(bytes));
    assert(timedFatStorage.getFailed());
    assert(timedFatStorage.closeSlot());
    assert(timedFatStorage.openSlot(0));
    MemoryRecordingDisk::failSync();
    assert(!timedFatStorage.closeSlot());
}

/* **
 * main - Exercise actual FatFs adapter control flow without a physical card.
 */
int main()
{
    checkStorage();
    std::puts("PASS: timed files through real ring/FatFs adapter, preservation and failure checks");
    return 0;
}
