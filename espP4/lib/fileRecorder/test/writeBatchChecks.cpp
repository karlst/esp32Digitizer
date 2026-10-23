// Copyright (C) 2026 Karl Stock
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file writeBatchChecks.cpp
 * Check real backend write sizes with the firmware's 128-KiB pump size.
 * RAM storage observes calls, not SD speed. This catches splitting each incoming
 * 30,720-byte batch into small unaligned writes, which the small-ring tests miss.
 */
#include "FileRecordingBackend.h"
#include "MemoryRecordingDisk.h"
#include <cstdlib>
#include <cstdio>
#include <stdexcept>

/* **
 * require - Stop on a failed invariant with readable context.
 */
static void require(bool ok, const char* message)
{
    if (!ok)
    {
        throw std::runtime_error(message);
    }
}

/* **
 * allocateRing - Allocate the backend's test ring in PC RAM.
 */
static uint8_t* allocateRing(uint32_t bytes)
{
    return static_cast<uint8_t*>(malloc(bytes));
}

/* **
 * releaseRing - Release PC RAM through the writer's normal ownership callback.
 */
static void releaseRing(uint8_t* memory)
{
    free(memory);
}

/* **
 * clockUs - Deterministic timer; this test makes no performance claim.
 */
static uint64_t clockUs()
{
    static uint64_t ticks = 0;
    return ++ticks;
}

/* **
 * utcSeconds - Fixed 2024 UTC for valid file headers.
 */
static uint64_t utcSeconds()
{
    return 1704067200ULL;
}

/* **
 * checkPayload - Confirm byte order across pump/file boundaries using the
 * accepted-stream offset.
 */
static bool checkPayload(const uint8_t* data, uint32_t length, uint64_t offset)
{
    bool retVal = true;
    for (uint32_t iByte = 0; retVal && iByte < length; ++iByte)
    {
        retVal = data[iByte] == static_cast<uint8_t>(offset + iByte);
    }
    return retVal;
}

/* **
 * checkBatches - Submit normal-size batches; inspect actual FatFs requests
 * before finalization. Large files require all 16-KiB writes. Smaller
 * sector-aligned files permit shorter boundary writes, but every request must
 * still be sector aligned.
 */
static void checkBatches(uint32_t capacity, uint32_t expectedChanges)
{
    MemoryRecordingDisk memoryRecordingDisk;
    FileRecordingBackend fileRecordingBackend;
    require(fileRecordingBackend.initializeMemory(allocateRing, releaseRing, clockUs, utcSeconds), "memory");
    RecordingSessionConfig recordingSessionConfig;
    recordingSessionConfig._preparation = RecordingSessionConfig::Preparation::formatAndAllocate;
    recordingSessionConfig._fullPolicy = RecordingSessionConfig::FullPolicy::overwriteOldest;
    recordingSessionConfig._secondsPerFile = 1;
    recordingSessionConfig._fileCount = 2;
    recordingSessionConfig._sampleRate = capacity / 3;
    recordingSessionConfig._bytesPerSample = 3;
    require(fileRecordingBackend.prepForRecording(memoryRecordingDisk, recordingSessionConfig), "prepare");
    MemoryRecordingDisk::clearWrites();

    std::vector<uint8_t> data(30720);
    uint64_t accepted = 0;
    for (unsigned batch = 0; batch < 32; ++batch)
    {
        for (uint32_t iByte = 0; iByte < data.size(); ++iByte)
        {
            data[iByte] = static_cast<uint8_t>(accepted + iByte);
        }
        require(fileRecordingBackend.copyToRingBuffer(data.data(), static_cast<uint32_t>(data.size())) == data.size(), "unexpected rejection");
        accepted += data.size();
        while (fileRecordingBackend.getQueuedBytes() >= BufferedWriter::_blockBytes)
        {
            require(fileRecordingBackend.pump(), "pump");
        }
    }
    require(fileRecordingBackend.finishRecording(), "finish");
    unsigned payloadWrites = 0, fullWrites = 0;
    for (const auto& write : MemoryRecordingDisk::getWrites())
    {
        if (write._offset >= 512)
        {
            ++payloadWrites;
            require(write._offset % 512 == 0 && write._length % 512 == 0, "unaligned payload write");
            if (write._length == 16384)
            {
                ++fullWrites;
            }
        }
    }
    if (expectedChanges == 0)
    {
        require(payloadWrites == 60 && fullWrites == 60, "incoming batches fragmented bulk writes");
    }
    else
    {
        require(fullWrites >= 50, "too few full-size writes across file boundaries");
    }
    const auto statistics = fileRecordingBackend.getStatistics();
    require(statistics._written == accepted && statistics._rollovers == expectedChanges, "written bytes/rollovers");
    require(fileRecordingBackend.verify(checkPayload), "payload readback");
}

/* **
 * main - PC entry, no physical filesystem or board access.
 */
int main()
{
    int retVal = 0;
    try
    {
        checkBatches(3145728, 0);
        checkBatches(460800, 2);
        puts("Session write batching checks passed");
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "Write batching FAIL: %s\n", error.what());
        retVal = 1;
    }
    return retVal;
}
