// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// PC checks of actual encoding and rotation code. No firmware or SD execution.
#include "TimedRecordingStream.h"
#include "MemoryTimedSink.h"
#include "recordingFormat.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>

/* **
 * readInteger - Inspect on-disk offsets independently of the encoder's helper.
 */
static uint64_t readInteger(const std::vector<uint8_t>& data, size_t offset, size_t bytes)
{
    uint64_t retVal = 0;
    for (size_t iByte = bytes; iByte > 0; --iByte)
    {
        retVal = retVal * 256 + data.at(offset + iByte - 1);
    }
    return retVal;
}

/* **
 * makeBlock - Make an explicit three-sample interval with extended timer counts.
 * Synthetic 100-tick seconds simplify assertions; this is not hardware timing.
 */
static TimeBlock makeBlock(uint32_t interval, uint16_t second)
{
    TimeBlock timeBlock;
    timeBlock._tcPpsFirst = 0xfffffff0ULL + uint64_t(interval) * 100;
    timeBlock._tcPpsLast = timeBlock._tcPpsFirst + 100;
    timeBlock._tcFsFirst = timeBlock._tcPpsFirst + 10;
    timeBlock._tcFsLast = timeBlock._tcPpsFirst + 20;
    timeBlock._sCount = 3;
    timeBlock._nSec = second;
    return timeBlock;
}

/* **
 * writeBlock - Send negative/zero/positive packed values in two batches. sBlock
 * boundaries need not coincide with decoder deliveries or physical SD chunks.
 */
static void writeBlock(TimedRecordingStream& timedRecordingStream, uint32_t interval, uint16_t second)
{
    const uint32_t words[] = {0x08ffffff, 0x08000000, 0x087fffff};
    uint8_t samples[9];
    assert(recordingFormat::packSamples(words, 3, samples, sizeof(samples)) == sizeof(samples));
    assert(timedRecordingStream.beginBlock(makeBlock(interval, second)));
    assert(timedRecordingStream.submitSamples(samples, 1));
    assert(timedRecordingStream.submitSamples(samples + 3, 2));
}

/* **
 * allBytes - Check reserved or marker bytes without relying on an encoder.
 */
static bool allBytes(const uint8_t* data, size_t length, uint8_t value)
{
    bool retVal = true;
    for (size_t iByte = 0; iByte < length; ++iByte)
    {
        retVal = retVal && data[iByte] == value;
    }
    return retVal;
}

/* **
 * checkLayout - Inspect literal offsets, reserved bytes and a no-header-rewrite
 * short file. Verify that invalid encoding leaves the supplied buffer untouched.
 */
static void checkLayout()
{
    MemoryTimedSink memoryTimedSink;
    TimedRecordingStream timedRecordingStream(memoryTimedSink);
    TimedRecordingConfig config;
    assert(config._pairsPerFile == 60 && config._fileCount == 10 && config._overwrite);
    assert(timedRecordingStream.start(1800000000, config));
    writeBlock(timedRecordingStream, 0, 0);
    assert(timedRecordingStream.finish());
    const auto& file = memoryTimedSink._files.at(0);
    assert(file.size() == 256 + 48 + 9 + 48);
    assert(std::memcmp(file.data(), "LXDG", 4) == 0 && readInteger(file, 4, 2) == 1);
    assert(readInteger(file, 6, 8) == 1800000000 && readInteger(file, 14, 4) == 2553);
    assert(readInteger(file, 18, 4) == 60);
    assert(allBytes(file.data() + 22, 234, 0));
    assert(readInteger(file, 256, 8) == 0xfffffffaULL);
    assert(readInteger(file, 264, 8) == 0x100000004ULL);
    assert(readInteger(file, 272, 8) == 0xfffffff0ULL);
    assert(readInteger(file, 280, 8) == 0x100000054ULL);
    assert(readInteger(file, 288, 4) == 3 && readInteger(file, 292, 2) == 0);
    assert(allBytes(file.data() + 294, 10, 0));
    assert(allBytes(file.data() + file.size() - 48, 48, 0));
    const uint8_t expectedSamples[] = {0xff, 0xff, 0xff, 0, 0, 0, 0xff, 0xff, 0x7f};
    assert(std::memcmp(file.data() + 304, expectedSamples, 9) == 0);

    uint8_t bytes[256];
    std::memset(bytes, 0xa5, sizeof(bytes));
    auto invalid = makeBlock(0, 0);
    invalid._sCount = 1;
    assert(!TimedFileFormat::encodeBlock(invalid, bytes, sizeof(bytes)));
    assert(allBytes(bytes, sizeof(bytes), 0xa5));
    assert(!TimedFileFormat::encodeHeader(TimedFileHeader{}, bytes, 255));
    auto shifted = makeBlock(0, 0);
    shifted._tcFsLast = shifted._tcPpsLast + 50;
    assert(TimedFileFormat::encodeBlock(shifted, bytes, sizeof(bytes)));
    shifted._tcFsFirst = shifted._tcPpsLast;
    assert(!TimedFileFormat::encodeBlock(shifted, bytes, sizeof(bytes)));
}

/* **
 * checkRotation - Rotate by pair count, resetting nSec and advancing header UTC.
 * Exercise slot reuse and a distinct stop-on-full configuration without changing
 * defaults. A completed file must not cause an empty successor to be created.
 */
static void checkRotation()
{
    for (bool overwrite : {false, true})
    {
        MemoryTimedSink memoryTimedSink;
        TimedRecordingStream timedRecordingStream(memoryTimedSink);
        TimedRecordingConfig config;
        config._pairsPerFile = 2;
        config._fileCount = 2;
        config._overwrite = overwrite;
        assert(timedRecordingStream.start(1000, config));
        for (uint32_t iInterval = 0; iInterval < 4; ++iInterval)
        {
            writeBlock(timedRecordingStream, iInterval, uint16_t(iInterval % 2));
        }
        assert(timedRecordingStream.getFull() == !overwrite);
        assert(memoryTimedSink._openedSlots == std::vector<uint32_t>({0, 1}));
        assert(readInteger(memoryTimedSink._files[1], 6, 8) == 1002);
        if (overwrite)
        {
            writeBlock(timedRecordingStream, 4, 0);
            assert(readInteger(memoryTimedSink._files[0], 6, 8) == 1004);
            assert(memoryTimedSink._openedSlots == std::vector<uint32_t>({0, 1, 0}));
        }
        assert(timedRecordingStream.finish());
        assert(!timedRecordingStream.getFailed());
    }
}

/* **
 * checkFailures - Reject gaps, incomplete intervals, oversized batches and sink
 * failures. Rejected work must not subsequently become a normally closed file.
 */
static void checkFailures()
{
    for (uint32_t rejectCall = 1; rejectCall <= 6; ++rejectCall)
    {
        MemoryTimedSink memoryTimedSink;
        memoryTimedSink._rejectCall = rejectCall;
        TimedRecordingStream timedRecordingStream(memoryTimedSink);
        assert(timedRecordingStream.start(0, TimedRecordingConfig{}));
        const uint8_t samples[9] = {};
        const bool accepted = timedRecordingStream.beginBlock(makeBlock(0, 0)) &&
            timedRecordingStream.submitSamples(samples, 3) && timedRecordingStream.finish();
        assert(!accepted && timedRecordingStream.getFailed());
        const auto calls = memoryTimedSink._calls;
        assert(!timedRecordingStream.finish());
        assert(memoryTimedSink._calls == calls);
    }
    MemoryTimedSink memoryTimedSink;
    TimedRecordingStream timedRecordingStream(memoryTimedSink);
    assert(timedRecordingStream.start(0, TimedRecordingConfig{}));
    assert(timedRecordingStream.beginBlock(makeBlock(0, 0)));
    assert(!timedRecordingStream.finish());
    assert(memoryTimedSink._closed == 0);

    MemoryTimedSink gapSink;
    TimedRecordingStream gapStream(gapSink);
    assert(gapStream.start(0, TimedRecordingConfig{}));
    writeBlock(gapStream, 0, 0);
    assert(!gapStream.beginBlock(makeBlock(2, 1)));
    assert(!gapStream.finish() && gapSink._closed == 0);
}

/* **
 * checkSampleSeconds - Filenames use the first saved analog sample, including
 * delay correction. Test both sides of the exact second boundary; formatting
 * must floor rather than round, and a pre-epoch result must be rejected.
 */
static void checkSampleSeconds()
{
    for (uint64_t ticks : {uint64_t(2552), uint64_t(2553), uint64_t(2554)})
    {
        MemoryTimedSink sink;
        TimedRecordingStream stream(sink);
        assert(stream.start(3600, TimedRecordingConfig{}));
        TimeBlock block;
        block._tcPpsFirst = 0;
        block._tcPpsLast = 1000000000;
        block._tcFsFirst = ticks;
        block._tcFsLast = ticks + 1;
        block._sCount = 2;
        assert(stream.beginBlock(block));
        assert(sink._sampleSeconds.size() == 1);
        assert(sink._sampleSeconds[0] == (ticks < 2553 ? 3599 : 3600));
    }

    MemoryTimedSink sink;
    TimedRecordingStream stream(sink);
    assert(stream.start(0, TimedRecordingConfig{}));
    TimeBlock block = makeBlock(0, 0);
    block._tcFsFirst = block._tcPpsFirst;
    assert(!stream.beginBlock(block));
    assert(sink._openedSlots.empty());
}

/* **
 * main - Run desktop-only assertions against the real format/rotation sources.
 */
int main()
{
    checkLayout();
    checkRotation();
    checkFailures();
    checkSampleSeconds();
    std::puts("PASS: DG layout, complete-block rotation, slot reuse and failure handling");
    return 0;
}
