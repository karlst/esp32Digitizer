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
 * @file decoderChecks.cpp
 * @brief Desktop synthetic-wire tests; no ADC, serial port or SD card is touched.
 * Build the real decoder with this independent wire encoder. Test all four
 * packing phases and all frame lengths, deliberately split DMA blocks, corrupt
 * headers, remove clock bits, and start mid-frame. Passing verifies software
 * interpretation only; physical timing and sustained throughput remain untested.
 */
#include <cassert>
#include <cstdio>
#include <vector>
#include "frameDecoder.h"
#include "sdTestData.h"
#include "recordingFormat.h"
#include "ads127BenchTiming.h"

/** @brief Read a test header's little-endian 64-bit field for boundary checks. */
static uint64_t header64(const uint8_t* input)
{
    uint64_t retVal = 0;
    for (unsigned index = 0; index < 8; ++index)
    {
        retVal |= static_cast<uint64_t>(input[index]) << (8 * index);
    }
    return retVal;
}

/** @brief Check signed 24-bit limits, status removal, bounds and both file formats. */
static void checkRecordingFormat()
{
    const uint32_t words[] = {0x08000000, 0x08ffffff, 0x087fffff, 0x08800000,
                              0x08123456, 0xff123456};
    const uint8_t expected[] = {0,0,0, 255,255,255, 255,255,127, 0,0,128,
                                0x56,0x34,0x12, 0x56,0x34,0x12};
    uint8_t bytes[20];
    std::memset(bytes, 0xaa, sizeof(bytes));
    assert(recordingFormat::packSamples(words, 6, bytes + 1, 18) == 18);
    assert(bytes[0] == 0xaa && bytes[19] == 0xaa);
    assert(std::memcmp(bytes + 1, expected, sizeof(expected)) == 0);
    const std::vector<uint8_t> before(bytes, bytes + sizeof(bytes));
    assert(recordingFormat::packSamples(words, 6, bytes + 1, 17) == 0);
    assert(std::vector<uint8_t>(bytes, bytes + sizeof(bytes)) == before);
    assert(recordingFormat::packSamples(words, 0, bytes, sizeof(bytes)) == 0);

    uint8_t header[512];
    recordingFormat::makeHeader(header, false, true, 0x12345678, 6, 1024000, 18, 0xabcdef01);
    assert(std::memcmp(header, "P4ADC03", 8) == 0);
    assert(header[8] == 3 && header[12] == 1);
    assert(header[16] == 0x78 && header[19] == 0x12);
    assert(header64(header + 28) == 18 && header[36] == 1 && header[39] == 0xab);
    assert(header[40] == 3 && header64(header + 44) == 6);
    for (size_t index = 52; index < sizeof(header); ++index) { assert(header[index] == 0); }
    for (uint64_t bytes : {0xffffffffULL, 0x100000000ULL, 5529600000ULL, 10000000000ULL})
    {
        recordingFormat::makeHeader(header, false, false, 1, 6, 1024000, bytes, 0xabcdef01);
        assert(header64(header + 28) == bytes && header64(header + 44) == bytes / 3);
        assert(header[36] == 1 && header[39] == 0xab && header[40] == 3 && header[12] == 0);
    }
    recordingFormat::makeHeader(header, true, true, 1, 0xffffffff, 812500, 20, 0);
    assert(std::memcmp(header, "P4SYN01", 8) == 0 && header[8] == 1);
    for (size_t index = 36; index < sizeof(header); ++index) { assert(header[index] == 0); }
    recordingFormat::makeHeader(header, false, false, 0, 0, 0, 0, 0);
    assert(header[12] == 0 && header64(header + 28) == 0 && header64(header + 44) == 0);
    assert(ads127BenchTiming::runMs == 1800000 && ads127BenchTiming::receiverTimeoutMs == 1805000);
    assert(uint64_t{1024000} * ads127BenchTiming::runMs / 1000 == 1843200000ULL);
    assert(uint64_t{3072000} * ads127BenchTiming::runMs / 1000 == 5529600000ULL);
    assert(uint64_t{8192000} * ads127BenchTiming::runMs / 1000 == 14745600000ULL);
    puts("PASS: three-byte packing, signed limits, 64-bit ADC-v3 headers and 30-minute bounds.");
}

/** @brief Check SD-test rates, duration cap and reproducible file words on the PC.
 * These arithmetic checks do not measure card speed or execute the P4 task.
 */
static void checkSdTestData()
{
    assert(sdTestData::dueWords(0, 812500) == 0);
    assert(sdTestData::dueWords(1000000, 812500) * 4 == 3250000);
    assert(sdTestData::dueWords(60000000, 812500) * 4 == 195000000);
    assert(sdTestData::dueWords(60000000, 1024000) * 4 == 245760000);
    assert(sdTestData::dueWords(90000000, 1024000) == 61440000);
    assert(sdTestData::dueWords(59999999, 1024000) == 61439998);
    uint64_t previous = 0;
    for (uint64_t elapsed = 0; elapsed < 61000000; elapsed += 1379)
    {
        const uint64_t due = sdTestData::dueWords(elapsed, 812500);
        assert(due >= previous && due <= 48750000);
        previous = due;
    }
    assert(sdTestData::word(0) == 0x9912ca41U);
    assert(sdTestData::word(1) == 0x99f46072U);
    puts("PASS: SD-only rate arithmetic, 60-second cap and known file pattern.");
}

/**
 * @brief Append one simulated frame: FSYNC high for half, word MSB first.
 * Remaining clocks carry arbitrary padding; they must not become extra samples.
 */
static void appendFrame(std::vector<unsigned>& pairs, unsigned clocks, uint32_t word)
{
    for (unsigned index = 0; index < clocks; ++index)
    {
        const unsigned data = index < 32 ? ((word >> (31 - index)) & 1U) : 1U;
        pairs.push_back((index < clocks / 2 ? 2U : 0U) | data);
    }
}

/** @brief Pack pairs in the documented PARLIO two-bit LSB layout; pad tail low. */
static std::vector<uint8_t> pack(const std::vector<unsigned>& pairs)
{
    std::vector<uint8_t> retVal((pairs.size() + 3) / 4, 0);
    for (size_t index = 0; index < pairs.size(); ++index)
    {
        retVal[index / 4] |= static_cast<uint8_t>(pairs[index] << ((index % 4) * 2));
    }
    return retVal;
}

/**
 * @brief Feed variable chunks, including one byte and odd descriptor sizes.
 * Hardware block boundaries must not affect framing, signed samples or counts.
 * A nonzero chunkSize selects a fixed split; memoryOffset deliberately misaligns
 * the first byte without feeding those leading storage bytes to the decoder.
 */
static void feedChunks(frameDecoder& decoder, const std::vector<unsigned>& pairs,
                       size_t chunkSize = 0, size_t memoryOffset = 0)
{
    auto bytes = pack(pairs);
    bytes.insert(bytes.begin(), memoryOffset, 0);
    size_t offset = memoryOffset;
    while (offset < bytes.size())
    {
        size_t count = chunkSize != 0 ? chunkSize : (offset % 71) + 1;
        if (count > bytes.size() - offset) { count = bytes.size() - offset; }
        decoder.feed(bytes.data() + offset, count);
        offset += count;
    }
}

/**
 * @brief Independent reference: find all frame starts, then extract each word.
 * This deliberately does not share the optimized decoder's streaming state or
 * lookup table. A frame shorter than 32 clocks is an error only if a following
 * rising edge cut it short; an incomplete final frame is a pending tail.
 */
static captureStats referenceDecode(std::vector<unsigned> pairs, unsigned frameClocks,
                                    uint32_t& pending)
{
    captureStats retVal;
    pending = 0;
    pairs.resize((pairs.size() + 3) / 4 * 4, 0);
    retVal.clocks = pairs.size();
    std::vector<size_t> starts;
    bool previousHigh = true;
    for (size_t index = 0; index < pairs.size(); ++index)
    {
        const bool high = (pairs[index] & 2U) != 0;
        if (high && !previousHigh) { starts.push_back(index); }
        previousHigh = high;
    }
    retVal.frames = static_cast<uint32_t>(starts.size());
    for (size_t frame = 0; frame < starts.size(); ++frame)
    {
        const bool hasNext = frame + 1 < starts.size();
        const size_t end = hasNext ? starts[frame + 1] : pairs.size();
        const size_t length = end - starts[frame];
        if (hasNext && length != frameClocks) { ++retVal.badPeriods; }
        if (length < 32)
        {
            if (hasNext) { ++retVal.shortWords; }
            else { pending = static_cast<uint32_t>(length); }
        }
        else
        {
            uint32_t word = 0;
            for (size_t bit = 0; bit < 32; ++bit)
            {
                word = word * 2 + (pairs[starts[frame] + bit] & 1U);
            }
            ++retVal.words;
            const unsigned header = word >> 24;
            ++retVal.headers[header];
            if ((header & 0x5fU) != 8)
            {
                if (retVal.badHeaders < 8) { retVal.badExamples[retVal.badHeaders] = word; }
                ++retVal.badHeaders;
            }
            retVal.latestWord = word;
            const int32_t sample = static_cast<int32_t>(word & 0xffffffU);
            retVal.latestSample = sample >= 0x800000 ? sample - 0x1000000 : sample;
        }
    }
    return retVal;
}

/**
 * @brief Compare every externally observable decoder field with the reference.
 * Generated bytes include valid words and deliberate FSYNC/data faults. Chunk
 * sizes vary so lookup paths and fallback paths must preserve state identically.
 */
static void checkReference(const std::vector<unsigned>& pairs, unsigned frameClocks,
                           size_t chunkSize = 0, size_t memoryOffset = 0)
{
    uint32_t expectedPending = 0;
    const auto expected = referenceDecode(pairs, frameClocks, expectedPending);
    frameDecoder decoder;
    decoder.reset(frameClocks);
    feedChunks(decoder, pairs, chunkSize, memoryOffset);
    const auto& actual = decoder.stats();
    assert(actual.clocks == expected.clocks && actual.frames == expected.frames);
    assert(actual.words == expected.words && actual.badHeaders == expected.badHeaders);
    assert(actual.badPeriods == expected.badPeriods && actual.shortWords == expected.shortWords);
    assert(actual.latestWord == expected.latestWord && actual.latestSample == expected.latestSample);
    assert(decoder.pendingBits() == expectedPending);
    for (unsigned index = 0; index < 256; ++index)
    {
        assert(actual.headers[index] == expected.headers[index]);
    }
    for (unsigned index = 0; index < 8; ++index)
    {
        assert(actual.badExamples[index] == expected.badExamples[index]);
    }
}

/** @brief Test-owned copy of submitted words, with an optional rejected batch. */
struct outputCheck
{
    std::vector<uint32_t> words;
    std::vector<uint8_t> packed;
    unsigned calls = 0;
    unsigned rejectCall = 0;
};

/** @brief Model a nonblocking destination that copies or refuses an entire batch. */
static bool collectOutput(const uint32_t* words, size_t count, void* context)
{
    auto& check = *static_cast<outputCheck*>(context);
    ++check.calls;
    const bool retVal = check.calls != check.rejectCall;
    assert(count > 0 && count <= 512);
    if (retVal)
    {
        check.words.insert(check.words.end(), words, words + count);
        uint8_t packed[512 * 3];
        const size_t bytes = recordingFormat::packSamples(words, count, packed, sizeof(packed));
        assert(bytes == count * 3);
        check.packed.insert(check.packed.end(), packed, packed + bytes);
    }
    return retVal;
}

/** @brief Verify every saved word, short final batches, refused output and recovery. */
static void checkOutput()
{
    std::vector<unsigned> pairs(7, 0);
    std::vector<uint32_t> expected;
    for (uint32_t index = 0; index < 1301; ++index)
    {
        // Include invalid headers and negative samples: recording preserves
        // the raw word while diagnostic counters still report their meaning.
        const uint32_t word = index * 1664525U + 1013904223U;
        expected.push_back(word);
        appendFrame(pairs, 32, word);
    }
    for (size_t chunk : {1U, 7U, 4096U, 20000U})
    {
        frameDecoder decoder;
        outputCheck output;
        decoder.setOutput(collectOutput, &output);
        decoder.reset(32);
        feedChunks(decoder, pairs, chunk);
        assert(output.words == expected && !decoder.outputFailed());
        assert(output.packed.size() == expected.size() * 3);
        for (size_t index = 0; index < expected.size(); ++index)
        {
            const uint32_t restored = output.packed[index * 3] |
                (static_cast<uint32_t>(output.packed[index * 3 + 1]) << 8) |
                (static_cast<uint32_t>(output.packed[index * 3 + 2]) << 16);
            assert(restored == (expected[index] & 0xffffff));
        }
        assert(decoder.stats().words == expected.size());
    }

    frameDecoder decoder;
    outputCheck output;
    output.rejectCall = 2;
    decoder.setOutput(collectOutput, &output);
    decoder.reset(32);
    feedChunks(decoder, pairs, 20000);
    assert(decoder.outputFailed() && output.calls == 2 && output.words.size() == 512);
    assert(decoder.stats().words == expected.size());
    for (size_t index = 0; index < output.words.size(); ++index)
    {
        assert(output.words[index] == expected[index]);
    }
    output = {};
    decoder.reset(32);
    feedChunks(decoder, pairs, 4096);
    assert(output.words == expected && !decoder.outputFailed());
    puts("PASS: all output words, batch boundaries, rejection latch and reset.");
}

/** @brief Exercise good streams, boundary failures, reset, and diagnostic limits. */
int main()
{
    checkRecordingFormat();
    checkSdTestData();
    checkOutput();
    // Make each varied payload the LAST word so reference comparison checks its
    // full 24-bit value, not just headers/histograms of earlier words. Sweep all
    // byte values and group phases to exercise every data lookup arrangement.
    for (unsigned phase = 0; phase < 16; ++phase)
    {
        for (uint32_t value = 0; value < 256; ++value)
        {
            std::vector<unsigned> pairs(4 + phase, 0);
            appendFrame(pairs, 32, (value << 24) | (value << 16) |
                ((255U - value) << 8) | value);
            checkReference(pairs, 32, 4, 1);
        }
    }

    // Exercise the sixteen possible rising-edge positions in a grouped read,
    // all four source-address alignments, and splits before/inside/after groups.
    // Both full-rate (32-clock) frames and slower frames use identical checks.
    for (unsigned slots : {1U, 2U, 4U, 8U})
    {
        for (unsigned phase = 0; phase < 16; ++phase)
        {
            std::vector<unsigned> pairs(4 + phase, 0);
            for (unsigned frame = 0; frame < 20; ++frame)
            {
                appendFrame(pairs, slots * 32, 0x08123456U + frame * 173U);
            }
            for (size_t alignment = 0; alignment < 4; ++alignment)
            {
                for (size_t chunk : {1U, 2U, 3U, 4U, 5U, 7U, 8U, 9U, 16U, 17U, 64U, 4032U, 4096U})
                {
                    checkReference(pairs, slots * 32, chunk, alignment);
                }
            }

            // Flip FSYNC at each position of a word. Some flips remove a rise;
            // others insert a second rise inside one group. Reference equality
            // ensures none of these unusual transitions vanish in the fast path.
            for (unsigned clock = 0; clock < 32; ++clock)
            {
                auto damaged = pairs;
                damaged[4 + phase + clock] ^= 2U;
                checkReference(damaged, slots * 32, 4, 1);
            }
        }
    }

    // Fixed arithmetic generator makes failures reproducible. This is test
    // stimulus, not a random-number quality or physical signal timing test.
    uint32_t generated = 0x9137abcd;
    for (unsigned slots : {1U, 2U, 4U, 8U})
    {
        for (unsigned phase = 0; phase < 4; ++phase)
        {
            std::vector<unsigned> pairs(4 + phase, 0);
            for (unsigned frame = 0; frame < 400; ++frame)
            {
                generated = generated * 1664525U + 1013904223U;
                appendFrame(pairs, slots * 32, generated);
            }
            checkReference(pairs, slots * 32);

            // Inject extra FSYNC edges, then missing clocks. All failures must
            // remain visible; optimization must not accidentally repair them.
            for (size_t index = 37; index < pairs.size(); index += 277)
            {
                pairs[index] ^= 2U;
            }
            pairs.erase(pairs.begin() + 101);
            checkReference(pairs, slots * 32);
            for (unsigned removed = 0; removed < 40; ++removed)
            {
                pairs.pop_back();
                checkReference(pairs, slots * 32);
            }
        }
    }

    // Cover every packed-byte value, including all within-byte FSYNC patterns.
    std::vector<unsigned> allPairs;
    for (unsigned packed = 0; packed < 256; ++packed)
    {
        for (unsigned shift = 0; shift < 8; shift += 2)
        {
            allPairs.push_back((packed >> shift) & 3U);
        }
    }
    checkReference(allPairs, 32);
    for (unsigned slots : {1U, 2U, 4U, 8U})
    {
        for (unsigned phase = 0; phase < 4; ++phase)
        {
            frameDecoder decoder;
            decoder.reset(slots * 32);
            std::vector<unsigned> pairs(4 + phase, 0);
            for (unsigned index = 0; index < 100; ++index)
            {
                appendFrame(pairs, slots * 32, 0x08123456U);
            }
            appendFrame(pairs, slots * 32, 0x08ffffffU);
            feedChunks(decoder, pairs);
            const auto& counts = decoder.stats();
            assert(counts.words == 101 && counts.frames == 101);
            assert(counts.badHeaders == 0 && counts.badPeriods == 0 && counts.shortWords == 0);
            assert(counts.headers[8] == 101 && counts.latestSample == -1);
            assert(decoder.pendingBits() == 0);

            // A fresh run must clear counters and discard an initially high
            // partial frame. The next real rising edge recovers alignment.
            decoder.reset(slots * 32);
            pairs.clear();
            appendFrame(pairs, slots * 32, 0x08123456U);
            appendFrame(pairs, slots * 32, 0x08800000U);
            feedChunks(decoder, pairs);
            assert(decoder.stats().words == 1);
            assert(decoder.stats().latestSample == -8388608);
        }
    }

    // Reject wrong channel, repeated data and ADC error flags. Preserve the
    // raw examples instead of silently shifting until a familiar header appears.
    frameDecoder decoder;
    decoder.reset(32);
    std::vector<unsigned> pairs(4, 0);
    appendFrame(pairs, 32, 0x09123456U);
    appendFrame(pairs, 32, 0x18123456U);
    appendFrame(pairs, 32, 0x48123456U);
    feedChunks(decoder, pairs);
    assert(decoder.stats().badHeaders == 3);
    assert(decoder.stats().badExamples[0] == 0x09123456U);

    // Remove a single clock in the first frame. Its next rising FSYNC is early;
    // recovery at that edge must retain the error and decode the following word.
    decoder.reset(32);
    pairs.assign(4, 0);
    appendFrame(pairs, 32, 0x08123456U);
    appendFrame(pairs, 32, 0x087fffffU);
    pairs.erase(pairs.begin() + 24);
    feedChunks(decoder, pairs);
    assert(decoder.stats().badPeriods == 1 && decoder.stats().shortWords == 1);
    assert(decoder.stats().latestSample == 8388607);

    // A final incomplete word is reported separately, never counted as complete.
    decoder.reset(32);
    pairs.assign(4, 0);
    appendFrame(pairs, 32, 0x08123456U);
    pairs.resize(16);
    feedChunks(decoder, pairs);
    assert(decoder.stats().words == 0 && decoder.pendingBits() == 12);

    // Deliberately change only payload: headers cannot detect this corruption.
    decoder.reset(32);
    pairs.assign(4, 0);
    appendFrame(pairs, 32, 0x08123456U);
    pairs[20] ^= 1;
    feedChunks(decoder, pairs);
    assert(decoder.stats().badHeaders == 0);
    assert(decoder.stats().latestWord != 0x08123456U);
    puts("PASS: packing phases, frame lengths, block splits, sign, reset, bad headers, missing clock and partial tail.");
    puts("PASS: independent reference comparison, varied payloads, FSYNC glitches, truncated streams and all packed-byte values.");
    puts("PASS: sixteen edge positions, unaligned input addresses and fixed group/block splits.");
    puts("These are PC decoder tests; they do not prove P4 DMA or ADC timing.");
    return 0;
}
