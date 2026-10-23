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
 * @file exactSampleChecks.cpp
 * Known digital samples -> synthetic PARLIO bytes -> real decoder -> RAM.
 * No board, analog voltage, UART, DMA engine or SD access. This tests software
 * interpretation and three-byte recording representation, not physical capture.
 * Read makeSamples(), encodeWire(), checkCase(), then main(). Expected values
 * come directly from the input list, not from another decoding implementation.
 */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "FrameDecoder.h"
#include "recordingFormat.h"

/* **
 * signedPattern - Convert a low-24-bit test pattern into its signed numeric
 * meaning.
 */
static int32_t signedPattern(uint32_t bits)
{
    const uint32_t low = bits & 0xffffffU;
    const int32_t retVal = static_cast<int32_t>(low) - ((low & 0x800000U) ? 0x1000000 : 0);
    return retVal;
}

/* **
 * makeSamples - Build Fibonacci values that fit signed 24 bits, then adversarial
 * values. Negative Fibonacci exercises sign handling. Walking-one/zero patterns
 * exercise every bit position; limits and mixed bytes catch sign/byte-order
 * mistakes. An additional deterministic sequence crosses decoder batch and DMA
 * boundaries.
 */
static std::vector<int32_t> makeSamples()
{
    std::vector<int32_t> retVal;
    int64_t previous = 1, current = 1;
    while (previous <= 8388607)
    {
        retVal.push_back(static_cast<int32_t>(previous));
        const int64_t next = previous + current;
        previous = current;
        current = next;
    }
    const size_t fibonacciCount = retVal.size();
    for (size_t iSample = 0; iSample < fibonacciCount; ++iSample)
    {
        retVal.push_back(-retVal[iSample]);
    }
    for (int32_t value : {0, -1, 8388607, -8388608, 0x123456, -0x123456})
    {
        retVal.push_back(value);
    }
    for (unsigned bit = 0; bit < 24; ++bit)
    {
        retVal.push_back(signedPattern(1U << bit));
        retVal.push_back(signedPattern(0xffffffU ^ (1U << bit)));
    }
    for (unsigned iSample = 0; iSample < 4097; ++iSample)
    {
        retVal.push_back(signedPattern(iSample * 0x10001U + 0x5a39c7U));
    }
    return retVal;
}

/* **
 * encodeWire - Independently serialize status 0x08 plus each sample, MSB first.
 * Each DCLK instant contributes DOUT in pair bit 0 and FSYNC in pair bit 1. Four
 * pairs occupy one byte, earliest pair in the least-significant two bits. Prefix
 * low clocks establish a real FSYNC rising edge at any of four packing phases.
 * In 64-clock frames, padding after the word must not become samples.
 */
static std::vector<uint8_t> encodeWire(const std::vector<int32_t>& samples,
    unsigned frameClocks, unsigned phase)
{
    const size_t totalClocks = 4 + phase + samples.size() * frameClocks;
    std::vector<uint8_t> retVal((totalClocks + 3) / 4, 0);
    size_t clock = 4 + phase;
    for (int32_t sample : samples)
    {
        const uint32_t payload = static_cast<uint32_t>(sample) & 0xffffffU;
        for (unsigned bit = 0; bit < frameClocks; ++bit, ++clock)
        {
            unsigned data = bit & 1U;
            if (bit < 8)
            {
                data = (0x08U >> (7 - bit)) & 1U;
            }
            else if (bit < 32)
            {
                data = (payload >> (31 - bit)) & 1U;
            }
            const unsigned pair = data | (bit < frameClocks / 2 ? 2U : 0U);
            retVal[clock / 4] |= static_cast<uint8_t>(pair << (2 * (clock % 4)));
        }
    }
    return retVal;
}

/* **
 * collectWords - Copy decoder batches before their backing array is reused, as
 * RAM evidence.
 */
static bool collectWords(const uint32_t* words, size_t count, void* context)
{
    auto& collected = *static_cast<std::vector<uint32_t>*>(context);
    collected.insert(collected.end(), words, words + count);
    return true;
}

/* **
 * require - Test comparisons stay active even if a build defines NDEBUG.
 */
static void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

/* **
 * mismatchAt - Return first differing index, or expected size when all values
 * match. Length differences are tested separately; no tolerance, fitting or
 * averaging.
 */
static size_t mismatchAt(const std::vector<int32_t>& expected, const std::vector<int32_t>& actual)
{
    size_t retVal = 0;
    while (retVal < expected.size() && retVal < actual.size() && expected[retVal] == actual[retVal])
    {
        ++retVal;
    }
    return retVal;
}

/* **
 * checkCase - Feed split raw buffers through the production decoder and packer.
 * mode tests one byte, three bytes, 4032 bytes, current DMA descriptor lengths,
 * or the whole stream. Memory offset separately tests unaligned source pointers.
 * Receiver block boundaries must not change any output value or its position.
 */
static void checkCase(const std::vector<int32_t>& expected, unsigned frameClocks,
    unsigned phase, unsigned memoryOffset, unsigned mode, bool show)
{
    auto wire = encodeWire(expected, frameClocks, phase);
    wire.insert(wire.begin(), memoryOffset, 0xa5);
    FrameDecoder frameDecoder;
    std::vector<uint32_t> words;
    frameDecoder.setOutput(collectWords, &words);
    frameDecoder.reset(frameClocks);
    const size_t descriptors[] = {4032, 4032, 4032, 4032, 4032, 4032, 4032, 2240, 2304};
    size_t position = memoryOffset, iDescriptor = 0;
    while (position < wire.size())
    {
        size_t length = wire.size() - position;
        if (mode == 0)
        {
            length = 1;
        }
        else if (mode == 1)
        {
            length = 3;
        }
        else if (mode == 2)
        {
            length = 4032;
        }
        else if (mode == 3)
        {
            length = descriptors[iDescriptor % 9];
        }
        length = std::min(length, wire.size() - position);
        frameDecoder.feed(wire.data() + position, length);
        position += length;
        ++iDescriptor;
    }
    require(words.size() == expected.size(), "decoded sample count differs (missing/extra output)");
    const auto& captureStats = frameDecoder.getCaptureStats();
    require(captureStats._words == expected.size() && captureStats._badHeaders == 0 && captureStats._badPeriods == 0 &&
                captureStats._shortWords == 0 && !frameDecoder.getOutputFailed(),
        "decoder counters report a fault");

    // Check raw status+sample words directly, before testing the recording packer.
    std::vector<int32_t> actual;
    for (size_t iSample = 0; iSample < words.size(); ++iSample)
    {
        require((words[iSample] >> 24) == 0x08, "status byte changed");
        actual.push_back(signedPattern(words[iSample]));
    }
    const size_t iMismatch = mismatchAt(expected, actual);
    if (iMismatch != expected.size())
    {
        std::fprintf(stderr, "Case frame=%u phase=%u memoryOffset=%u split=%u: sample %llu expected=%ld actual=%ld\n",
            frameClocks, phase, memoryOffset, mode, static_cast<unsigned long long>(iMismatch),
            static_cast<long>(expected[iMismatch]), static_cast<long>(actual[iMismatch]));
    }
    require(iMismatch == expected.size(), "decoded sample value/order differs");

    // Exercise the exact three-byte storage representation in RAM only. Compare
    // individual bytes with the original numeric input; no file or SD operation.
    std::vector<uint8_t> packed(words.size() * 3);
    require(recordingFormat::packSamples(words.data(), words.size(), packed.data(), packed.size()) == packed.size(),
        "recording packer returned the wrong byte count");
    for (size_t iSample = 0; iSample < expected.size(); ++iSample)
    {
        const uint32_t original = static_cast<uint32_t>(expected[iSample]) & 0xffffffU;
        for (unsigned byte = 0; byte < 3; ++byte)
        {
            require(packed[iSample * 3 + byte] == ((original >> (byte * 8)) & 0xffU),
                "three-byte recording representation changed a sample");
        }
    }
    if (show)
    {
        std::puts("First Fibonacci values (input -> decoded output):");
        for (size_t iSample = 0; iSample < 8; ++iSample)
        {
            std::printf("  %ld -> %ld\n", static_cast<long>(expected[iSample]), static_cast<long>(actual[iSample]));
        }
    }
}

/* **
 * checkChangedWireBit - Flip one simulated data bit and require the exact
 * comparison to notice. Keep status and FSYNC untouched: header counters alone
 * cannot detect this sample-data change. The known expected sequence must reveal
 * it at index 10.
 */
static void checkChangedWireBit(const std::vector<int32_t>& expected)
{
    auto wire = encodeWire(expected, 32, 1);
    const size_t clock = 5 + 10 * 32 + 31;
    wire[clock / 4] ^= static_cast<uint8_t>(1U << (2 * (clock % 4)));
    FrameDecoder frameDecoder;
    std::vector<uint32_t> words;
    frameDecoder.setOutput(collectWords, &words);
    frameDecoder.reset(32);
    frameDecoder.feed(wire.data(), wire.size());
    require(words.size() == expected.size(), "bit-change control unexpectedly changed sample count");
    std::vector<int32_t> actual;
    for (uint32_t word : words)
    {
        actual.push_back(signedPattern(word));
    }
    require(mismatchAt(expected, actual) == 10, "known-sequence check missed a changed wire data bit");
    require(frameDecoder.getCaptureStats()._badHeaders == 0, "bit-change control accidentally altered a header");
}

/* **
 * main - Run all boundary cases plus deliberately altered sequence checks.
 */
int main()
{
    const auto samples = makeSamples();
    checkChangedWireBit(samples);
    auto damaged = samples;
    damaged[100] ^= 1;
    require(mismatchAt(samples, damaged) == 100, "comparator failed to detect a one-bit change");
    damaged = samples;
    std::swap(damaged[3], damaged[4]);
    require(mismatchAt(samples, damaged) == 3, "comparator failed to detect reordered samples");
    damaged = samples;
    damaged.erase(damaged.begin() + 10);
    require(damaged.size() != samples.size() && mismatchAt(samples, damaged) == 10,
        "comparator failed to detect a dropped sample");
    unsigned cases = 0;
    for (unsigned clocks : {32U, 64U})
    {
        for (unsigned phase = 0; phase < 4; ++phase)
        {
            for (unsigned offset = 0; offset < 4; ++offset)
            {
                for (unsigned mode = 0; mode < 5; ++mode)
                {
                    checkCase(samples, clocks, phase, offset, mode, cases == 0);
                    ++cases;
                }
            }
        }
    }
    std::printf("PASS: %llu samples matched EXACTLY in each of %u cases, including three-byte packing.\n",
        static_cast<unsigned long long>(samples.size()), cases);
    std::puts("Fibonacci, negative values, 24-bit limits, walking bits, alignment and split buffers checked.");
    std::puts("Software-only result: physical ADC output, PARLIO hardware and DMA are NOT tested here.");
    return 0;
}
