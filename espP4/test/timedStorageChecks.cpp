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
    timedFatStorage.setTimingClock(clockUs);
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

    // Preparation and readback must not inflate recording-operation counters.
    // Five one-pair files reuse slots 0,1,0,1,0. Each has one open and close.
    const auto& timings = timedFatStorage.getTimings();
    assert(timings.size() == 2);
    for (unsigned iSlot = 0; iSlot < 2; ++iSlot)
    {
        const auto& operations = timings[iSlot]._operations;
        assert(operations[unsigned(TimedFatStorage::Operation::open)]._calls == (iSlot == 0 ? 3 : 2));
        assert(operations[unsigned(TimedFatStorage::Operation::close)]._calls == (iSlot == 0 ? 3 : 2));
        assert(operations[unsigned(TimedFatStorage::Operation::rename)]._calls == (iSlot == 0 ? 3 : 2));
        assert(operations[unsigned(TimedFatStorage::Operation::rename)]._failures == 0);
        const auto& writes = operations[unsigned(TimedFatStorage::Operation::write)];
        assert(writes._calls > 0 && writes._totalUs == writes._calls && writes._maxUs == 1);
        assert(writes._requested == writes._actual && writes._failures == 0);
    }
    assert(MemoryRecordingDisk::files().at("0:/important.txt") == std::vector<uint8_t>({1, 2, 3, 4}));
    for (uint32_t iSlot = 0; iSlot < 2; ++iSlot)
    {
        // Delay correction moves these samples just before their opening PPS.
        // The last retained generations are intervals 4 and 3, not slot IDs.
        const char* name = iSlot == 0 ? "/LXD19700101-001643.bin" : "/LXD19700101-001642.bin";
        assert(std::string(timedFatStorage.getPath(iSlot)) == std::string(timedFatStorage.getDirectory()) + name);
        const auto& file = MemoryRecordingDisk::files().at(timedFatStorage.getPath(iSlot));
        assert(file.size() == 4096);
        assert(std::memcmp(file.data(), memoryTimedSink._files[iSlot].data(), memoryTimedSink._files[iSlot].size()) == 0);
    }

    // Preparation creates another directory, leaving the completed session intact.
    const auto saved = MemoryRecordingDisk::files();
    assert(timedFatStorage.prepare(memoryRecordingDisk, 1, 4096));
    assert(timedFatStorage.getTimings().size() == 1);
    assert(timedFatStorage.getTimings()[0]._operations[unsigned(TimedFatStorage::Operation::write)]._calls == 0);
    for (const auto& entry : saved)
    {
        assert(MemoryRecordingDisk::files().at(entry.first) == entry.second);
    }
    assert(timedFatStorage.openSlot(0, 3600));
    const uint8_t bytes[20] = {};
    MemoryRecordingDisk::failWrite(true);
    assert(timedFatStorage.write(bytes, sizeof(bytes)) == sizeof(bytes));
    assert(timedFatStorage.getPendingBytes() == sizeof(bytes));
    assert(!timedFatStorage.flush());
    assert(timedFatStorage.getFailed());
    const auto& failedWrite = timedFatStorage.getTimings()[0]._operations[unsigned(TimedFatStorage::Operation::write)];
    assert(failedWrite._calls == 1 && failedWrite._failures == 1);
    assert(failedWrite._offset == 0 && failedWrite._requested == sizeof(bytes));
    assert(failedWrite._actual < failedWrite._requested);
    assert(timedFatStorage.getPendingBytes() == sizeof(bytes) - failedWrite._actual);
    assert(!timedFatStorage.closeSlot());
    assert(!timedFatStorage.openSlot(0, 3600));
    assert(!timedFatStorage.readback(checkReadback, &memoryTimedSink));
    assert(timedFatStorage.prepare(memoryRecordingDisk, 1, 4096));
    assert(timedFatStorage.openSlot(0, 3600));
    MemoryRecordingDisk::failSync();
    assert(!timedFatStorage.closeSlot());
    assert(timedFatStorage.getTimings()[0]._operations[unsigned(TimedFatStorage::Operation::sync)]._failures == 1);
}

/* **
 * checkAlignment - Feed two files through deliberately awkward caller chunks.
 * Full SD writes must still begin at 16-KiB offsets, followed by one short
 * tail per file. Readback compares all bytes to detect padding or carryover.
 */
static void checkAlignment()
{
    MemoryRecordingDisk memoryRecordingDisk;
    TimedFatStorage timedFatStorage;
    assert(timedFatStorage.initializeMemory(allocateBytes, releaseBytes));
    assert(timedFatStorage.prepare(memoryRecordingDisk, 2, 65536));
    MemoryTimedSink expected;
    expected._files.resize(2);
    MemoryRecordingDisk::clearWrites();
    for (unsigned iSlot = 0; iSlot < 2; ++iSlot)
    {
        auto& bytes = expected._files[iSlot];
        bytes.resize(50007);
        for (size_t iByte = 0; iByte < bytes.size(); ++iByte)
        {
            bytes[iByte] = uint8_t(iByte + iSlot * 17);
        }
        assert(timedFatStorage.openSlot(iSlot, 3600 + iSlot));
        assert(timedFatStorage.write(bytes.data(), 123) == 123);
        assert(timedFatStorage.write(bytes.data() + 123, 17001) == 17001);
        assert(timedFatStorage.write(bytes.data() + 17124, 32883) == 32883);
        assert(timedFatStorage.getPendingBytes() == 855);
        assert(timedFatStorage.closeSlot());
        assert(timedFatStorage.getPendingBytes() == 0);
    }
    const auto& writes = MemoryRecordingDisk::getWrites();
    assert(writes.size() == 8);
    for (unsigned iWrite = 0; iWrite < writes.size(); ++iWrite)
    {
        assert(writes[iWrite]._offset == (iWrite % 4) * 16384);
        assert(writes[iWrite]._length == (iWrite % 4 == 3 ? 855 : 16384));
    }
    assert(timedFatStorage.readback(checkReadback, &expected));

    // A full-block failure during write must be latched even when the return
    // count says all caller bytes were copied. Close must not retry that block.
    assert(timedFatStorage.prepare(memoryRecordingDisk, 1, 65536));
    assert(timedFatStorage.openSlot(0, 3600));
    MemoryRecordingDisk::clearWrites();
    MemoryRecordingDisk::failWrite(true);
    assert(timedFatStorage.write(expected._files[0].data(), 16384) == 16384);
    assert(timedFatStorage.getFailed());
    assert(timedFatStorage.getPendingBytes() > 0);
    assert(!timedFatStorage.closeSlot());
    assert(MemoryRecordingDisk::getWrites().size() == 1);
}

/* **
 * checkRenameFailures - Refuse collisions and disk failures without losing
 * preallocation, old contents or unrelated destination files. No write follows
 * a failed rename; the failure stays latched until a new preparation.
 */
static void checkRenameFailures()
{
    MemoryRecordingDisk disk;
    TimedFatStorage storage;
    storage.setTimingClock(clockUs);
    assert(storage.initializeMemory(allocateBytes, releaseBytes));
    assert(storage.prepare(disk, 1, 4096));
    const std::string original = storage.getPath(0);
    const std::string collision = std::string(storage.getDirectory()) + "/LXD19700101-010000.bin";
    MemoryRecordingDisk::files()[collision] = {9, 8, 7};
    const auto before = MemoryRecordingDisk::files();
    assert(!storage.openSlot(0, 3600));
    assert(storage.getFailed() && std::string(storage.getPath(0)) == original);
    assert(MemoryRecordingDisk::files() == before);
    assert(storage.getTimings()[0]._operations[unsigned(TimedFatStorage::Operation::rename)]._failures == 1);

    assert(storage.prepare(disk, 1, 4096));
    const auto beforeFailure = MemoryRecordingDisk::files();
    MemoryRecordingDisk::failRename();
    assert(!storage.openSlot(0, 3600));
    assert(storage.getFailed() && MemoryRecordingDisk::files() == beforeFailure);
    assert(!storage.openSlot(0, 3601));

    // UTC calendar formatting must handle midnight and leap day on both PCs
    // and firmware. Renaming the same slot again must retain its full capacity.
    assert(storage.prepare(disk, 1, 4096));
    assert(storage.openSlot(0, 951782400));
    assert(std::string(storage.getPath(0)).find("LXD20000229-000000.bin") != std::string::npos);
    assert(storage.closeSlot());
    assert(storage.openSlot(0, 951868800));
    assert(std::string(storage.getPath(0)).find("LXD20000301-000000.bin") != std::string::npos);
    assert(MemoryRecordingDisk::files().at(storage.getPath(0)).size() == 4096);
    assert(storage.closeSlot());
}

/* **
 * main - Exercise actual FatFs adapter control flow without a physical card.
 */
int main()
{
    checkStorage();
    checkAlignment();
    checkRenameFailures();
    std::puts("PASS: timed files through real ring/FatFs adapter, preservation and failure checks");
    return 0;
}
