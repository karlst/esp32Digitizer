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
 * @file chokeTest.h
 * @brief Timed dummy-sample source for measuring the existing recording pipeline.
 */
#pragma once
#include <bufferedWriter.h>
#ifndef S3_CHOKE_INITIAL_BPS
#if S3_CHOKE_FIXED_RATE
#define S3_CHOKE_INITIAL_BPS 250000
#else
#define S3_CHOKE_INITIAL_BPS 750000
#endif
#endif
#ifndef S3_CHOKE_DURATION_SECONDS
#define S3_CHOKE_DURATION_SECONDS 0
#endif

/**
 * @brief Generate signed 24-bit values stored as four-byte words, without an ADC.
 * The acquisition task on core 0 owns this object. feed() copies due samples into
 * the normal ring; the independent disk task on core 1 still calls pump(). There
 * is no disk access, sleeping, or second consumer here. Desktop tests supply a
 * simulated microsecond clock to verify the ramp without waiting ten seconds.
 */
class chokeTest
{
public:
    // Fixed-rate builds hold initialBps: 250 kbps by default for the bus-width
    // comparison, or an explicit rate for endurance. Ordinary Choke builds ramp.
#if S3_CHOKE_FIXED_RATE
    static constexpr bool fixedRate = true;
#else
    static constexpr bool fixedRate = false;
#endif
    static constexpr uint32_t initialBps = S3_CHOKE_INITIAL_BPS;

    // Zero means run until Stop or failure. A timed test stops on the S3 itself,
    // even if the USB monitor disconnects; accepted bytes then drain normally.
    static constexpr uint64_t durationUs = uint64_t(S3_CHOKE_DURATION_SECONDS) * 1000000;
    static constexpr uint32_t incrementBps = 250000;

    // Exact integer pacing below uses units of 250 kbps (one word per 128 us).
    static_assert(initialBps > 0 && initialBps % incrementBps == 0,
        "Initial rate must be a positive multiple of 250 kbps");
    static constexpr uint64_t stageUs = 10000000;

    // All rates count stored bits: 32 per sample, including the sign-extension byte.
    // producerBehind is distinct from card/ring failure: a delayed generator must
    // not dump an unlimited catch-up burst and falsely blame the card for choking.
    enum resultCode : uint32_t { idle, running, stopped, storageLimit, producerBehind, rateLimit };
    void start(uint64_t nowUs);
    bool feed(uint64_t nowUs, bufferedWriter& writer);
    void stop(resultCode reason);
    uint32_t targetBps = initialBps;
    // Last ramp stage whose due data entered RAM, or the final rate after a timed
    // run. This alone does not confirm that storage saved the queued bytes.
    uint32_t completedBps = 0;
    uint32_t elapsedMs = 0;
    uint64_t samples = 0; // Successfully copied into the ring, not necessarily written yet.
    int32_t latestSample = 0;
    resultCode result = idle;
private:
    uint64_t startedUs = 0;

    // Bound one feed call so Stop commands get serviced between short batches.
    // At 24 Mbps a 1-ms scheduler interval needs only 750 words, below this limit.
    static constexpr uint32_t maxDueSamples = 4096;
    uint8_t block[1024] = {}; // At most 256 words per ring submission; not a disk buffer.
};
