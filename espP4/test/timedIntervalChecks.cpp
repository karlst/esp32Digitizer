// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// Desktop checks for the actual staging/formatting classes, without hardware.
#include "TimedIntervalBuffer.h"
#include "MemoryTimedSink.h"
#include <cassert>
#include <cstdio>
#include <vector>

/* **
 * boundary - Produce illustrative metadata after each synthetic one-second PPS.
 * A short 100-sample interval makes wrap/rotation visible with a small ring;
 * it does not model the physical ADC rate. Tick values cross 32-bit rollover.
 */
static TimedBoundary boundary(unsigned iBoundary)
{
    const uint64_t pps = 4294967200ULL + uint64_t(iBoundary) * 160000000;
    return {pps, pps + 844, pps + 1000, 5 + uint64_t(iBoundary) * 100};
}

/* **
 * submit - Encode absolute sample indices as three-byte little-endian values.
 * Readback can then detect one missing, repeated, reordered or extra sample.
 */
static void submit(TimedIntervalBuffer& buffer, unsigned first, unsigned count)
{
    std::vector<uint8_t> packed(count * 3);
    for (unsigned iSample = 0; iSample < count; ++iSample)
    {
        const unsigned value = first + iSample;
        packed[iSample * 3] = uint8_t(value);
        packed[iSample * 3 + 1] = uint8_t(value >> 8);
        packed[iSample * 3 + 2] = uint8_t(value >> 16);
    }
    assert(buffer.submit(packed.data(), count));
}

/* **
 * read - Decode an expected field independently of the production encoder.
 */
static uint64_t read(const std::vector<uint8_t>& bytes, size_t position, unsigned count)
{
    uint64_t retVal = 0;
    for (unsigned iByte = 0; iByte < count; ++iByte)
    {
        retVal |= uint64_t(bytes.at(position + iByte)) << (8 * iByte);
    }
    return retVal;
}

/* **
 * main - Exercise late samples, late boundaries, startup/tail omission, repeated
 * ring wrap and file rotation. Inject overflow, invalid timing and sink refusal.
 * These are software checks; concurrent scheduling and SD throughput need P4 tests.
 */
int main()
{
    MemoryTimedSink sink;
    TimedRecordingStream stream(sink);
    TimedRecordingConfig config;
    config._pairsPerFile = 3;
    config._fileCount = 4;
    config._overwrite = false;
    assert(stream.start(946684800, config));
    uint8_t memory[256 * 3];
    TimedIntervalBuffer buffer;
    assert(buffer.start(memory, 256, stream, 3));
    assert(buffer.addBoundary(boundary(0)));
    assert(buffer.addBoundary(boundary(1)));
    assert(!buffer.pump());
    submit(buffer, 0, 50);
    assert(buffer.pump()); // Only the five startup samples can be discarded yet.
    assert(sink._files.empty());
    submit(buffer, 50, 55);
    while (buffer.pump())
    {
        // Drain only completed intervals; no hardware or disk work is running.
    }
    assert(buffer.getIntervals() == 1);
    for (unsigned iInterval = 1; iInterval < 12; ++iInterval)
    {
        submit(buffer, 5 + iInterval * 100, 100);
        assert(!buffer.pump()); // No closing timestamp yet.
        assert(buffer.addBoundary(boundary(iInterval + 1)));
        while (buffer.pump())
        {
            // Small copies exercise physical ring wrap without changing order.
        }
        assert(!buffer.getFailed());
    }
    submit(buffer, 1205, 20); // Deliberately incomplete shutdown interval.
    assert(!buffer.pump());
    assert(buffer.finish());
    assert(buffer.getIntervals() == 12 && buffer.getSamples() == 1200);
    assert(sink._closed == 4);
    for (unsigned iFile = 0; iFile < 4; ++iFile)
    {
        const auto& bytes = sink._files[iFile];
        assert(bytes.size() == 256 + 3 * (48 + 300) + 48);
        assert(read(bytes, 4, 8) == 946684800 + iFile * 3);
        for (unsigned iBlock = 0; iBlock < 3; ++iBlock)
        {
            const unsigned iInterval = iFile * 3 + iBlock;
            const size_t position = 256 + iBlock * 348;
            assert(read(bytes, position, 8) == boundary(iInterval)._tcFsAfter);
            assert(read(bytes, position + 8, 8) == boundary(iInterval + 1)._tcFsBefore);
            assert(read(bytes, position + 16, 8) == boundary(iInterval)._tcPps);
            assert(read(bytes, position + 24, 8) == boundary(iInterval + 1)._tcPps);
            assert(read(bytes, position + 32, 4) == 100);
            assert(read(bytes, position + 36, 2) == iBlock);
            for (unsigned iSample = 0; iSample < 100; ++iSample)
            {
                assert(read(bytes, position + 48 + iSample * 3, 3) == 5 + iInterval * 100 + iSample);
            }
        }
        for (size_t iByte = bytes.size() - 48; iByte < bytes.size(); ++iByte)
        {
            assert(bytes[iByte] == 0);
        }
    }

    // Fresh fixtures isolate each failure; none may create a normal end marker.
    for (unsigned scenario = 0; scenario < 4; ++scenario)
    {
        MemoryTimedSink failingSink;
        TimedRecordingStream failingStream(failingSink);
        TimedIntervalBuffer failingBuffer;
        assert(failingStream.start(946684800, config));
        assert(failingBuffer.start(memory, 256, failingStream, 3));
        if (scenario == 0)
        {
            submit(failingBuffer, 0, 256);
            const uint8_t extra[3] = {};
            assert(!failingBuffer.submit(extra, 1));
        }
        else if (scenario == 1)
        {
            auto invalid = boundary(0);
            invalid._tcFsAfter += 156;
            assert(!failingBuffer.addBoundary(invalid));
        }
        else
        {
            assert(failingBuffer.addBoundary(boundary(0)));
            auto closing = boundary(1);
            if (scenario == 2)
            {
                closing._tcPps += 160000000;
                closing._tcFsBefore += 160000000;
                closing._tcFsAfter += 160000000;
            }
            else
            {
                failingSink._rejectCall = 3; // Refuse the first tBlock.
            }
            assert(failingBuffer.addBoundary(closing));
            submit(failingBuffer, 0, 105);
            failingBuffer.pump();
        }
        assert(failingBuffer.getFailed());
        assert(!failingBuffer.finish());
        assert(failingSink._closed == 0);
    }
    std::puts("PASS: timed staging, exact sample continuity, wrap, rotation and faults");
    return 0;
}
