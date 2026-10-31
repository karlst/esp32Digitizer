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
 * @file SdSmokeSummary.cpp
 * Reconstruct ADC readings from SD bytes and group them into 100 ms sections.
 * Read beginRange, accept, getComplete and report in that order. File headers
 * have already been removed; each three input bytes represent one ADC reading.
 * Time positions are calculated using the configured sample rate, not a second
 * clock. Statistics and serial reporting run only after acquisition stops.
 */
#include "SdSmokeSummary.h"
#include <cstdio>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

/* **
 * pauseReport - Give other Core 0 tasks time to run between serial report rows.
 * The disk task can otherwise keep printing long enough to starve the idle
 * task, which the watchdog checks to detect a CPU stuck doing one job. Sleep
 * one scheduler tick (FreeRTOS's scheduling time unit), allowing the idle task
 * to run even though it has lower priority than the disk task. Capture has already
 * stopped. PC arithmetic tests have no FreeRTOS scheduler and need no pause.
 */
static void pauseReport()
{
#ifdef ESP_PLATFORM
    vTaskDelay(1);
#endif
}

/* **
 * reset - Clear the counters before reading a recording. samplesPerSecond is
 * the number of ADC readings per second. beginRange calls this before setting
 * the actual readback range; direct PC tests can use the default 30-second range.
 */
void SdSmokeSummary::reset(uint32_t samplesPerSecond)
{
    _rate = samplesPerSecond;
    _firstSample = _totalSamples = 0;
    _iFirstBin = 0;
    _bins = 300;
    _range = false;
    _samples = _clipped = 0;
    _waveformCount = 0;
    _partial = _partialBytes = 0;
    for (unsigned iBin = 0; iBin < 801; ++iBin)
    {
        _sums[iBin] = 0;
        _counts[iBin] = 0;
        _origins[iBin] = _minimums[iBin] = _maximums[iBin] = 0;
        _deltaSums[iBin] = 0;
        _deltaSquares[iBin] = 0;
        if (iBin < _traceBins)
        {
            _traceCounts[iBin] = 0;
        }
    }
}

/* **
 * accept - Read bytes from the supplied buffer and reconstruct ADC readings.
 * The disk readback callback supplies data and its length in bytes. Keep any
 * unfinished three-byte reading for the next call; do not keep the data pointer.
 *
 * The lowest byte comes first. Assemble an unsigned value, then subtract 2^24
 * if its sign bit is set: bytes 00 00 80 become -8388608. Update the 100 ms bin
 * statistics and save selected individual readings for graphs. Count complete
 * readings even beyond the expected range so getComplete can detect extras.
 */
void SdSmokeSummary::accept(const uint8_t* data, size_t bytes)
{
    for (size_t iByte = 0; iByte < bytes; ++iByte)
    {
        _partial |= static_cast<uint32_t>(data[iByte]) << (8 * _partialBytes);
        ++_partialBytes;
        if (_partialBytes == 3)
        {
            const int32_t value = (_partial & 0x800000U) != 0
                                      ? static_cast<int32_t>(_partial) - 16777216
                                      : static_cast<int32_t>(_partial);
            const uint64_t iBin = _rate != 0 ? (_firstSample + _samples) * 10 / _rate - _iFirstBin : 801;
            if (_rate != 0 && _samples >= _rate && _samples - _rate < _waveformSamples)
            {
                _waveform[_waveformCount++] = value;
            }
            if (iBin < _bins && iBin < 801)
            {
                // Compute individual-sample spread while reading disk, never
                // during acquisition. Subtract the bin's first code before
                // squaring to avoid subtracting two huge DC-voltage squares.
                if (_counts[iBin] == 0)
                {
                    _origins[iBin] = _minimums[iBin] = _maximums[iBin] = value;
                }
                const int64_t delta = int64_t(value) - _origins[iBin];
                _deltaSums[iBin] += delta;
                _deltaSquares[iBin] += static_cast<double>(delta * delta);
                if (value < _minimums[iBin])
                {
                    _minimums[iBin] = value;
                }
                if (value > _maximums[iBin])
                {
                    _maximums[iBin] = value;
                }

                // Store a small consecutive window, not every Nth sample.
                // Absolute bin position keeps traces correctly timed even when
                // readback starts partway through a bin after circular reuse.
                const uint64_t position = _firstSample + _samples -
                    (_iFirstBin + iBin) * (_rate / 10);
                const uint32_t traceStart = _rate / 20;
                if (iBin < _traceBins && position >= traceStart &&
                    position - traceStart < _traceSamples)
                {
                    _traces[iBin][_traceCounts[iBin]++] = value;
                }
                _sums[iBin] += value;
                ++_counts[iBin];
            }
            if (value == -8388608 || value == 8388607)
            {
                ++_clipped;
            }
            ++_samples;
            _partial = _partialBytes = 0;
        }
    }
}

/* **
 * getComplete - Require complete retained sample/bin counts and no saturated
 * codes. Legacy reset-only tests retain their 30-second duration check. Range
 * mode validates every retained sample; PC analysis checks selected duration
 * against the whole-run total. CRC and receiver errors are checked by the
 * recorder. A disconnected but steady signal can pass these structural checks;
 * the PC must also compare against the measured input.
 */
bool SdSmokeSummary::getComplete() const
{
    bool retVal = _rate == 1024000 && _partialBytes == 0 && _clipped == 0;
    if (_range)
    {
        retVal = retVal && _bins <= 801 && _totalSamples > _firstSample &&
                 _samples == _totalSamples - _firstSample;
        for (unsigned iBin = 0; retVal && iBin < _bins; ++iBin)
        {
            const uint64_t lower = uint64_t(_iFirstBin + iBin) * (_rate / 10);
            const uint64_t upper = lower + _rate / 10;
            const uint64_t begin = lower > _firstSample ? lower : _firstSample;
            const uint64_t end = upper < _totalSamples ? upper : _totalSamples;
            retVal = end >= begin && _counts[iBin] == end - begin;
        }
    }
    else
    {
        retVal = retVal && _samples >= uint64_t{_rate} * 30 - _rate / 50 &&
                 _samples <= uint64_t{_rate} * 30 + _rate / 50;
        for (unsigned iBin = 0; iBin < 300; ++iBin)
        {
            if (_counts[iBin] < _rate * 8 / 100)
            {
                retVal = false;
            }
        }
    }
    return retVal;
}

/* **
 * getMean - Return the average ADC reading in 100 ms bin iBin, before conversion
 * to volts. Return zero for an empty bin or an index outside the stored array.
 */
double SdSmokeSummary::getMean(unsigned iBin) const
{
    const double retVal = iBin < 801 && _counts[iBin] != 0
                              ? static_cast<double>(_sums[iBin]) / _counts[iBin]
                              : 0.0;
    return retVal;
}

/* **
 * getSquaredDeviations - Return the sum of squared distances from this bin's
 * mean, in ADC counts squared. Divide by sample count for population variance.
 * The PC combines settled bins, including differences between their means.
 */
double SdSmokeSummary::getSquaredDeviations(unsigned iBin) const
{
    double retVal = 0;
    if (iBin < 801 && _counts[iBin] != 0)
    {
        const double sum = static_cast<double>(_deltaSums[iBin]);
        retVal = _deltaSquares[iBin] - sum * sum / _counts[iBin];
        if (retVal < 0)
        {
            retVal = 0; // Floating-point rounding cannot imply negative variance.
        }
    }
    return retVal;
}

/* **
 * getMinimum - Return the smallest individual ADC code in a bin, or zero if absent.
 */
int32_t SdSmokeSummary::getMinimum(unsigned iBin) const
{
    return iBin < 801 ? _minimums[iBin] : 0;
}

/* **
 * getMaximum - Return the largest individual ADC code in a bin, or zero if absent.
 */
int32_t SdSmokeSummary::getMaximum(unsigned iBin) const
{
    return iBin < 801 ? _maximums[iBin] : 0;
}

/* **
 * getTraceSample - Return one retained raw code for the PC checks; absent is zero.
 */
int32_t SdSmokeSummary::getTraceSample(unsigned iBin, unsigned iSample) const
{
    return iBin < _traceBins && iSample < _traceCounts[iBin] ? _traces[iBin][iSample] : 0;
}

/* **
 * getSampleCount - Return the number of complete ADC readings received by
 * accept, including any extra readings beyond the requested range.
 */
uint64_t SdSmokeSummary::getSampleCount() const
{
    return _samples;
}

/* **
 * getWaveformSample - Return a raw sample in the five-ms readback window for
 * PC arithmetic checks. A missing or out-of-range sample returns zero.
 */
int32_t SdSmokeSummary::getWaveformSample(unsigned iSample) const
{
    return iSample < _waveformCount ? _waveform[iSample] : 0;
}

/* **
 * report - Print the readback statistics and selected individual samples after
 * recording and file verification finish. run identifies the acquisition;
 * storageClean is the caller's combined storage/acquisition result. The printed
 * clean flag also requires getComplete to pass. The PC checks the report and
 * waveform before deciding PASS; this function does not make that decision.
 */
void SdSmokeSummary::report(uint32_t run, bool storageClean) const
{
    if (_range)
    {
        std::printf("SMOKE_SD_RANGE firstSample=%llu retainedSamples=%llu totalSamples=%llu\n",
            static_cast<unsigned long long>(_firstSample), static_cast<unsigned long long>(_samples),
            static_cast<unsigned long long>(_totalSamples));
    }
    std::printf("SMOKE_SD_BEGIN run=%lu rate=%lu samples=%llu bins=%u clean=%u\n",
        static_cast<unsigned long>(run), static_cast<unsigned long>(_rate),
        static_cast<unsigned long long>(_range ? _totalSamples : _samples), _bins, unsigned(storageClean && getComplete()));
    for (unsigned iBin = 0; iBin < _bins && iBin < 801; ++iBin)
    {
        pauseReport();
        std::printf("SMOKE_SD,%u,%lu,%.6f\n", _iFirstBin + iBin,
            static_cast<unsigned long>(_counts[iBin]), getMean(iBin));
    }

    // Each SMOKE_MOMENT row supplies the squared-deviation sum, minimum and
    // maximum for the corresponding 100 ms bin. The PC selects flat sections
    // later. Short SMOKE_TRACE rows illustrate individual samples; they do not
    // limit which samples contribute to a bin's statistics.
    std::printf("SMOKE_NOISE version=1 traceSamples=%u traceBins=%u\n", _traceSamples, _traceBins);
    for (unsigned iBin = 0; iBin < _bins && iBin < 801; ++iBin)
    {
        pauseReport();
        std::printf("SMOKE_MOMENT,%u,%.9g,%ld,%ld\n", _iFirstBin + iBin,
            getSquaredDeviations(iBin), static_cast<long>(_minimums[iBin]), static_cast<long>(_maximums[iBin]));
        if (iBin < _traceBins && _traceCounts[iBin] == _traceSamples)
        {
            std::printf("SMOKE_TRACE,%u", _iFirstBin + iBin);
            for (unsigned iSample = 0; iSample < _traceSamples; ++iSample)
            {
                std::printf(",%ld", static_cast<long>(_traces[iBin][iSample]));
            }
            std::printf("\n");
        }
    }

    // Include the first sample index in each 64-sample row so the PC can detect
    // missing or repeated rows. Pause between rows to let the idle task run;
    // otherwise the watchdog can reset the board during this long report.
    pauseReport();
    std::printf("SMOKE_RAW_BEGIN version=1 firstSample=%llu count=%u rate=%lu\n",
        static_cast<unsigned long long>(_firstSample + _rate), _waveformCount,
        static_cast<unsigned long>(_rate));
    for (unsigned iSample = 0; iSample < _waveformCount; iSample += _reportChunkSamples)
    {
        pauseReport();
        std::printf("SMOKE_RAW,%u", iSample);
        for (unsigned iChunkSample = 0; iChunkSample < _reportChunkSamples && iSample + iChunkSample < _waveformCount; ++iChunkSample)
        {
            std::printf(",%ld", static_cast<long>(_waveform[iSample + iChunkSample]));
        }
        std::printf("\n");
    }
    std::printf("SMOKE_RAW_END count=%u\n", _waveformCount);
    std::printf("SMOKE_SD_END run=%lu\n", static_cast<unsigned long>(run));
}

/* **
 * beginRange - Begin readback at an absolute sample offset after circular
 * overwrite. The backend supplies the accepted total and oldest surviving
 * sample. Bins keep their original 100-ms positions so the PC can match
 * Feather's retained bursts. Fixed storage covers at most 80 seconds plus a
 * partial end bin.
 */
void SdSmokeSummary::beginRange(uint32_t rate, uint64_t firstSample, uint64_t totalSamples)
{
    reset(rate);
    _range = true;
    _firstSample = firstSample;
    _totalSamples = totalSamples;
    _iFirstBin = rate ? static_cast<unsigned>(firstSample * 10 / rate) : 0;
    const uint64_t endBin = rate ? (totalSamples * 10 + rate - 1) / rate : 0;
    _bins = endBin >= _iFirstBin && endBin - _iFirstBin <= 801 ? static_cast<unsigned>(endBin - _iFirstBin) : 802;
}
