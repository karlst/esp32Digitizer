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
 * @file hardwareSmokeChecks.cpp
 * PC-only checks of real disk-byte parsing and square-wave timing.
 * This does not exercise SD hardware, the DAC, or ADC voltage accuracy. Karl
 * runs the test runner before building the two opt-in firmware environments.
 */
#include "SdSmokeSummary.h"
#include "RecordingLayout.h"
#include "SyntheticRecordingLayout.h"
#include "benchDacWave.h"
#include "test/support/ads127BenchTiming.h"
#include <cassert>
#include <cstdio>

/* **
 * checkRecordingLayouts - Verify that selecting a file policy preserves count
 * checks and file formats. Calls through the base interface model the recorder's
 * dispatch. The synthetic policy must find a corrupted word read back at a
 * nonzero file offset; the normal ADC policy must reject a mismatch between
 * accepted bytes and decoded samples. These checks use memory only and cannot
 * establish successful SD operations.
 */
static void checkRecordingLayouts()
{
    RecordingLayout recordingLayout;
    SyntheticRecordingLayout syntheticRecordingLayout;
    const RecordingLayout& recordingLayoutSynthetic = syntheticRecordingLayout;
    assert(recordingLayout.getRecordBytes() == 3);
    assert(recordingLayout.countMatches(12, 4));
    assert(!recordingLayout.countMatches(11, 4));
    assert(!recordingLayout.countMatches(12, 5));
    assert(recordingLayoutSynthetic.getRecordBytes() == 4);
    assert(recordingLayoutSynthetic.countMatches(16, 0));
    assert(!recordingLayoutSynthetic.countMatches(15, 0));

    // Build a small interior slice, as a disk read would provide it.
    uint8_t payload[16] = {};
    for (unsigned iByte = 0; iByte < 4; ++iByte)
    {
        const uint32_t word = sdTestData::word(17 + iByte);
        for (unsigned byte = 0; byte < 4; ++byte)
        {
            payload[iByte * 4 + byte] = static_cast<uint8_t>(word >> (byte * 8));
        }
    }
    assert(recordingLayoutSynthetic.check(payload, sizeof(payload), 17 * 4) == 0);
    payload[5] ^= 1;
    assert(recordingLayoutSynthetic.check(payload, sizeof(payload), 17 * 4) == 1);

    // Historical single-file helpers remain for old captures. Current recording
    // uses SessionFileHeader, exercised by the separate backend checks.
    uint8_t header[512] = {};
    recordingFormat::makeHeader(header, true, 7, 1024000, 12, 123);
    assert(std::memcmp(header, "P4ADC03", 8) == 0);
    sdTestData::makeHeader(header, true, 7, 1024000, 16, 123);
    assert(std::memcmp(header, "P4SYN01", 8) == 0);
}

/* **
 * main - Check byte splits, sign, full duration, and rejection of incomplete
 * tails.
 */
int main()
{
    checkRecordingLayouts();
    assert(ads127BenchTiming::runMs == 30000);
    assert(ads127BenchTiming::receiverTimeoutMs == 35000);
    assert(benchDacWave::codeAt(0) == 110);
    assert(benchDacWave::codeAt(999999) == 110);
    assert(benchDacWave::codeAt(1000000) == 135);
    assert(benchDacWave::codeAt(2000000) == 110);
    assert(benchDacWave::codeAt(29999999) == 135);

    // One sample per bin exposes sign extension and two separate read splits.
    SdSmokeSummary sdSmokeSummary;
    sdSmokeSummary.reset(10);
    const uint8_t signedValues[] = {255, 255, 255, 0, 0, 128, 255, 255, 127, 1, 0, 0};
    sdSmokeSummary.accept(signedValues, 1);
    sdSmokeSummary.accept(signedValues + 1, 3);
    sdSmokeSummary.accept(signedValues + 4, 8);
    assert(sdSmokeSummary.getSampleCount() == 4);
    assert(sdSmokeSummary.getMean(0) == -1);
    assert(sdSmokeSummary.getMean(1) == -8388608);
    assert(sdSmokeSummary.getMean(2) == 8388607);
    assert(sdSmokeSummary.getMean(3) == 1);
    assert(!sdSmokeSummary.getComplete());

    // Feed a complete 30-second file in awkward 4093-byte reads (not divisible
    // by three). This crosses both sample and 100-ms bin boundaries repeatedly.
    sdSmokeSummary.reset(1024000);
    uint8_t block[4093] = {};
    const uint64_t bytes = uint64_t{1024000} * 30 * 3;
    for (uint64_t offset = 0; offset < bytes;)
    {
        const size_t amount = bytes - offset < sizeof(block)
                                  ? static_cast<size_t>(bytes - offset)
                                  : sizeof(block);
        for (size_t iByte = 0; iByte < amount; ++iByte)
        {
            const uint64_t position = offset + iByte;
            const uint32_t code = ((position / 3) / 1024000) % 2 == 0 ? 1000000 : 2000000;
            block[iByte] = static_cast<uint8_t>(code >> (8 * (position % 3)));
        }
        sdSmokeSummary.accept(block, amount);
        offset += amount;
    }
    assert(sdSmokeSummary.getComplete());
    assert(sdSmokeSummary.getSampleCount() == 30720000);
    for (unsigned iByte = 0; iByte < 300; ++iByte)
    {
        assert(sdSmokeSummary.getMean(iByte) == (iByte / 10 % 2 == 0 ? 1000000 : 2000000));
    }
    sdSmokeSummary.accept(block, 1);
    assert(!sdSmokeSummary.getComplete());

    // Retained readback keeps absolute time and permits partial edge bins.
    // Only 103 samples survive here; this isolates indexing from long captures.
    const uint64_t firstSample = uint64_t{224} * 1024000 + 102350;
    sdSmokeSummary.beginRange(1024000, firstSample, firstSample + 103);
    uint8_t retained[309] = {};
    sdSmokeSummary.accept(retained, 308);
    assert(!sdSmokeSummary.getComplete());
    sdSmokeSummary.accept(retained + 308, 1);
    assert(sdSmokeSummary.getComplete());
    assert(sdSmokeSummary.getSampleCount() == 103);
    sdSmokeSummary.beginRange(1024000, 0, uint64_t{82} * 1024000);
    assert(!sdSmokeSummary.getComplete());
    sdSmokeSummary.reset(1024000);
    assert(sdSmokeSummary.getSampleCount() == 0 && !sdSmokeSummary.getComplete());
    std::puts("PASS: SD byte boundaries, signed samples, duration, square-wave timing and incomplete-file rejection");
    return 0;
}
