/* Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
 * Check timestamp spacing without assuming the two oscillator frequencies are exact.
 * Read extendCount for hardware counter wrap, then analyze for sample mapping.
 * This is ordinary arithmetic shared by firmware and PC tests: no device calls,
 * allocation, interrupts or logging. An anchor is a saved pulse-number/time pair.
 */
#include "FsyncAssociationAnalysis.h"
#include <cmath>

/* **
 * extendCount - Attach a recently read small PCNT register to the driver's
 * accumulated count. Hardware resets to zero every 32767 pulses. For example,
 * baseline 32766 and hardwareCount 2 means pulse 32769, not pulse 2. The caller
 * must bound elapsed time to 100 us at 1.024 MSPS, much less than a counter wrap.
 * Reject invalid registers or an advance above 2048 pulses; zero denotes an
 * unusable observation (including no pulse yet). No hardware is accessed here.
 */
uint32_t FsyncAssociationAnalysis::extendCount(uint32_t baseline, int hardwareCount)
{
    uint32_t retVal = 0;
    if (hardwareCount >= 0 && hardwareCount < 32767)
    {
        const uint32_t advance = (uint32_t(hardwareCount) + 32767 - baseline % 32767) % 32767;
        if (advance <= 2048)
        {
            retVal = baseline + advance;
        }
    }
    return retVal;
}

/* **
 * analyze - Called after collection by the firmware or synthetic PC checks.
 * Pulse counts are one-based. With one discarded startup frame, count 2 belongs
 * to word index 0; generally index = count - discardedFrames - 1. Only timestamps
 * whose words were retained are tested. Fit spacing from the first and last
 * retained observations, then check every interior observation to within four
 * 160-MHz ticks (25 ns). This is a diagnostic consistency threshold, not a
 * claim of 25-ns absolute timing accuracy. A constant numbering error shared by
 * every observation cannot be detected here; startup checks must establish zero.
 * Unsigned subtraction accepts one timer wrap during this sub-second test.
 */
FsyncAssociationAnalysis::Result FsyncAssociationAnalysis::analyze(
    const Anchor* anchors, size_t count, uint32_t words, uint32_t discardedFrames)
{
    Result retVal;

    // Reject unordered observations before using any pair to estimate spacing.
    // words is the number retained in RAM, not every pulse observed at shutdown.
    bool valid = anchors != nullptr && count >= 4 && discardedFrames <= 1;
    for (size_t iAnchor = 0; valid && iAnchor < count; ++iAnchor)
    {
        const auto& anchor = anchors[iAnchor];
        valid = anchor._pulseCount != 0;
        if (iAnchor != 0)
        {
            const auto& previous = anchors[iAnchor - 1];
            const uint32_t elapsed = anchor._ticks - previous._ticks;
            valid = valid && anchor._pulseCount > previous._pulseCount &&
                elapsed != 0 && elapsed < 160000000U;
        }
        if (valid && anchor._pulseCount <= discardedFrames)
        {
            ++retVal._firstMatched;
        }
        else if (valid && anchor._pulseCount - discardedFrames <= words)
        {
            ++retVal._matched;
        }
    }

    if (!valid)
    {
        retVal._matched = 0;
    }

    // Counts are increasing, so retained observations form one contiguous range
    // after any discarded startup pulse and before any undelivered tail. Require
    // enough spread to distinguish one sample (about 156 ticks) from rounding.
    if (valid && retVal._matched >= 4)
    {
        const auto& first = anchors[retVal._firstMatched];
        const auto& last = anchors[retVal._firstMatched + retVal._matched - 1];
        const uint32_t sampleSpan = last._pulseCount - first._pulseCount;
        const uint32_t tickSpan = last._ticks - first._ticks;

        // Two anchors 1024 sample intervals apart should span about 160000
        // timer ticks. Dividing gives about 156.25 ticks per sample. This
        // measured ratio allows the ADC and P4 oscillators to differ slightly.
        retVal._ticksPerSample = double(tickSpan) / sampleSpan;
        valid = sampleSpan >= 1024 && retVal._ticksPerSample > 156.0 &&
            retVal._ticksPerSample < 156.5;
        for (size_t iAnchor = 0; iAnchor < retVal._matched; ++iAnchor)
        {
            // Residual means the observed tick count minus the count predicted
            // by that straight line. Only interior anchors test the prediction;
            // the two endpoints define it and therefore have zero residual.
            const auto& anchor = anchors[retVal._firstMatched + iAnchor];
            const double predicted = (anchor._pulseCount - first._pulseCount) * retVal._ticksPerSample;
            const double error = std::abs(double(uint32_t(anchor._ticks - first._ticks)) - predicted);
            if (error > retVal._maxResidualTicks)
            {
                retVal._maxResidualTicks = error;
            }
        }
        retVal._passed = valid && retVal._maxResidualTicks <= 4.0;
    }
    return retVal;
}
