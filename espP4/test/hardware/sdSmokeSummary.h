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
 * recorder checksum check. This class does not decide analog accuracy.
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
    uint64_t getSampleCount() const;
    void report(uint32_t run, bool storageClean) const;

private:
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
