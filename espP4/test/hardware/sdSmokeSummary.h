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
 * @file SdSmokeSummary.h
 * Calculate waveform and noise statistics from samples read back from SD.
 * SdRecorder's disk task (a separately scheduled thread) calls this object after
 * recording stops. accept receives bytes returned by f_read, not live samples.
 * A bin is a 100 ms group of samples. For each bin, save its count, sum, smallest
 * and largest reading, and information needed to calculate variance. The PC
 * selects flat high/low sections after locating the voltage transitions.
 * The recorder separately checks the readback checksums. This class does not
 * decide whether the measured voltage is accurate or impose a noise limit.
 */
#pragma once
#include "RecordingReadback.h"
#include <cstddef>
#include <cstdint>

/**
 * Reconstruct signed ADC readings, each stored as three bytes, during readback.
 * SdRecorder::finishFile calls beginRange, which resets the counters, then calls
 * accept through its readback callback, and finally report to print the results.
 * A reading may span two calls: bytes FF FF at the end of one read followed by
 * FF in the next read form the signed reading -1.
 * At most 801 bins cover the retained 80 seconds plus a partial bin.
 * Save 128 consecutive readings at the center of each of the first 40 bins.
 * A second 5120-sample window covers five ms one second into retained data.
 * Optional grounded-input mode sends one full second during readback in small
 * rows. Only a 385-byte text buffer is added; the million readings stay on SD
 * until read. The PC, not this class, calculates the FFT and noise verdict.
 * Extra fixed storage is about 63 KiB, in this test-only object; no ring-buffer
 * memory is consumed and no sample statistics run during acquisition.
 * This object allocates no memory. Optional serial reporting waits for output
 * and yields CPU time, only after capture has stopped. It uses each supplied
 * read buffer only during accept; SdRecorder may reuse it afterwards.
 */
class SdSmokeSummary : public RecordingReadback
{
public:
    void setNoiseReporting(bool enabled);
    void reset(uint32_t samplesPerSecond);
    void beginRange(uint32_t rate, uint64_t firstSample, uint64_t totalSamples) override;
    void accept(const uint8_t* data, size_t bytes);
    bool getComplete() const;
    double getMean(unsigned iBin) const;
    double getSquaredDeviations(unsigned iBin) const;
    int32_t getMinimum(unsigned iBin) const;
    int32_t getMaximum(unsigned iBin) const;
    int32_t getTraceSample(unsigned iBin, unsigned iSample) const;
    int32_t getWaveformSample(unsigned iSample) const;
    uint64_t getSampleCount() const;
    void report(uint32_t run, bool storageClean) const;

private:
    void reportNoiseSample(uint32_t packedValue);
    bool _noiseReporting = false;
    uint32_t _noiseCount = 0;
    uint32_t _noiseCrc = 0xffffffffU;
    // Hex sends the original three bytes without a megabyte-sized staging buffer.
    static constexpr unsigned _noiseChunkSamples = 64;
    char _noiseHex[_noiseChunkSamples * 6 + 1] = {};

    // Five milliseconds at the test's 1,024,000 samples/s. Read one second
    // into retained SD data, after startup; retain consecutive samples, not averages.
    static constexpr unsigned _waveformSamples = 5120;
    static constexpr unsigned _reportChunkSamples = 64;
    int32_t _waveform[_waveformSamples] = {};
    unsigned _waveformCount = 0;

    // Forty 100 ms bins cover two cycles of the 0.5 Hz test signal, so short
    // traces can show both levels even when recording begins mid-cycle.
    // Each trace holds 128 consecutive readings starting at the bin center.
    static constexpr unsigned _traceBins = 40;
    static constexpr unsigned _traceSamples = 128;
    int32_t _traces[_traceBins][_traceSamples] = {};
    unsigned _traceCounts[_traceBins] = {};
    int32_t _origins[801] = {}, _minimums[801] = {}, _maximums[801] = {};
    int64_t _deltaSums[801] = {};
    double _deltaSquares[801] = {};
    int64_t _sums[801] = {};
    uint32_t _counts[801] = {};
    uint64_t _samples = 0;
    uint64_t _firstSample = 0, _totalSamples = 0;
    unsigned _iFirstBin = 0, _bins = 300;
    bool _range = false;
    uint32_t _rate = 0, _partial = 0;
    unsigned _partialBytes = 0;
    uint64_t _clipped = 0;
};
