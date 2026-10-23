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
 * @file DecoderTiming.h
 * Per-block timing measurements used by the P4 bench receiver.
 * This lightweight timing is retained in both existing builds. captureProfile
 * separately enables the more expensive task/CPU statistics. The receiver owns
 * clock reads and scheduler decisions; this object only accumulates and reports.
 */
#pragma once
#include <cstdint>
#include <cstdio>

/**
 * Core 1's timing totals for one capture, read after all queued blocks finish.
 * decodeUs measures time inside decoder.feed(); pauseUs measures deliberate
 * scheduling pauses. bytes and blocks count raw input processed by that task.
 * Times are elapsed microseconds, including time when another task interrupts
 * this one. Reading the clock also costs time: these are approximate timings,
 * not a direct measurement of processor instruction cycles.
 */
struct DecoderTiming
{
    uint64_t _decodeUs = 0;
    uint64_t _pauseUs = 0;
    uint64_t _bytes = 0;
    uint32_t _blocks = 0;
    uint32_t _pauses = 0;
    int64_t _maxDecodeUs = 0;
    int64_t _maxWaitUs = 0;

    /* **
     * recordWait - Record a block's queue delay in microseconds; called by Core 1.
     */
    void recordWait(int64_t elapsedUs)
    {
        if (elapsedUs > _maxWaitUs)
        {
            _maxWaitUs = elapsedUs;
        }
    }

    /* **
     * recordDecode - Add one decoder call's elapsed microseconds and raw input
     * bytes.
     */
    void recordDecode(int64_t elapsedUs, uint32_t rawBytes)
    {
        _decodeUs += elapsedUs;
        if (elapsedUs > _maxDecodeUs)
        {
            _maxDecodeUs = elapsedUs;
        }
        _bytes += rawBytes;
        ++_blocks;
    }

    /* **
     * recordPause - Record an already-completed scheduler pause; this does not
     * delay.
     */
    void recordPause(int64_t elapsedUs)
    {
        _pauseUs += elapsedUs;
        ++_pauses;
    }

    /* **
     * report - Print timings on Core 0 after queued decoding has finished. rate is
     * ADC samples/second; slots describes raw frame length. Callback gaps and copy
     * times are microseconds measured by the receive interrupt. These elapsed-time
     * estimates include interruptions, not just CPU work.
     */
    void report(uint32_t rate, uint32_t slots, int64_t maxCallbackGapUs, int64_t maxCopyUs) const
    {
        // MB/s uses decimal megabytes. decodeMBps measures feed() wall time only;
        // it excludes queue operations and deliberate pauses, so it is an upper
        // bound on whole-worker throughput. Compare with requiredRawMBps, not the
        // smaller useful ADC payload rate. Maximum times expose occasional stalls.
        printf("TIMING decodeMBps=%.3f requiredRawMBps=%.3f decodeAvgUs=%.1f "
               "decodeMaxUs=%lld queueWaitMaxUs=%lld\n",
            _decodeUs != 0 ? static_cast<double>(_bytes) / _decodeUs : 0.0,
            rate *
                slots * 8.0 / 1000000.0,
            _blocks != 0 ? static_cast<double>(_decodeUs) / _blocks : 0.0,
            static_cast<long long>(_maxDecodeUs), static_cast<long long>(_maxWaitUs));
        printf("TIMING callbackGapMaxUs=%lld copyMaxUs=%lld pauses=%lu pauseTotalUs=%llu\n",
            static_cast<long long>(maxCallbackGapUs), static_cast<long long>(maxCopyUs),
            static_cast<unsigned long>(_pauses), static_cast<unsigned long long>(_pauseUs));
    }
};
