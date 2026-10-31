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
 * Summarize actual SD readback bytes for selectable-duration hardware tests.
 * The SD writer task owns this object after capture stops. accept() receives
 * only bytes returned by f_read, never the live decoder or its RAM samples.
 * Each bin is a 100-ms arithmetic average; all bytes still undergo the normal
 * recorder checksum check. Bin moments and extrema retain individual-sample
 * spread. The PC selects settled bins after identifying square-wave edges.
 * This class does not decide analog accuracy or impose a noise limit.
 */
#pragma once
#include "RecordingReadback.h"
#include <cstddef>
#include <cstdint>

/**
 * Fixed-memory, streaming parser for signed three-byte ADC samples.
 * reset(), accept(), report() are called in that order by sdRecorder::finishFile.
 * A partial sample survives read boundaries: FF FF followed by FF means -1.
 * At most 801 bins cover the retained 80 seconds plus a partial bin.
 * Short raw traces cover bin centers in the first four retained seconds.
 * A second 5120-sample window covers five ms one second into retained data.
 * Extra fixed storage is about 63 KiB, in this test-only object; no ring-buffer
 * memory is consumed and no sample statistics run during acquisition.
 * No allocation or hardware access occurs; the caller owns read-buffer memory.
 */
class SdSmokeSummary : public RecordingReadback
{
public:
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
    // Five milliseconds at the test's 1,024,000 samples/s. Read one second
    // into retained SD data, after startup; retain consecutive samples, not averages.
    static constexpr unsigned _waveformSamples = 5120;
    static constexpr unsigned _reportChunkSamples = 64;
    int32_t _waveform[_waveformSamples] = {};
    unsigned _waveformCount = 0;
    // Four seconds of short traces ensure both levels are available regardless
    // of generator phase. Each trace holds 128 consecutive samples at bin center.
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
