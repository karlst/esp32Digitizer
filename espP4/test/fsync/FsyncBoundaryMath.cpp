// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// Number a captured adjacent FSYNC pair without reading hardware. The P4's PPS
// boundary search and PC checks use this same integer arithmetic. Read advance
// in the header for pair selection, then resolve below for numbering and guards.
#include "FsyncBoundaryMath.h"
#include <limits>

/* **
 * resolve - Accept only consecutive captures 156 or 157 timer ticks apart.
 * At 160 MHz / 1.024 MSPS, one sample is nominally 156.25 ticks; observing a
 * two-sample jump cannot pass this guard. Number the earlier edge relative to
 * the supplied anchor, requiring a gap <=10 ms and <0.25-sample residual.
 * referencePulse/referenceTicks describe a recent numbered FSYNC, not the PPS.
 * beforeTicks/afterTicks are hardware captures, not interpolated estimates.
 * Success fills boundary with both ticks and the later pulse number; failure
 * leaves it unchanged. This checks arithmetic and spacing, not startup's choice
 * of pulse number or the GPS time of the PPS. Unsigned subtraction handles one
 * raw timer wrap, provided the elapsed-gap limit remains satisfied.
 */
bool FsyncBoundaryMath::resolve(uint32_t referencePulse, uint32_t referenceTicks,
    uint32_t beforeTicks, uint32_t afterTicks, FsyncBoundary& boundary)
{
    const uint32_t spacing = afterTicks - beforeTicks;
    const uint32_t elapsed = beforeTicks - referenceTicks;

    // A sample period is 625/4 ticks. Scaling elapsed ticks by four permits
    // nearest-integer rounding with (scaled+312)/625. For 160000 elapsed ticks,
    // pulses is 1024. error is in quarter-tick units; 156/625 = 0.2496 sample,
    // just below the 0.25-sample rejection threshold used for numbering.
    const uint64_t scaled = uint64_t(elapsed) * 4;
    const uint64_t pulses = (scaled + 312) / 625;
    const uint64_t rounded = pulses * 625;
    const uint64_t error = scaled > rounded ? scaled - rounded : rounded - scaled;
    const bool retVal = referencePulse >= 2 && elapsed <= 1600000 &&
        (spacing == 156 || spacing == 157) && error <= 156 &&
        pulses + referencePulse + 1 <= std::numeric_limits<uint32_t>::max();
    if (retVal)
    {
        // pulses counts from the reference to the EARLIER edge. Add one for
        // the later edge, which starts the new sBlock. Under startup's rule,
        // its sample index will be this pulse number minus two.
        boundary = {beforeTicks, afterTicks, uint32_t(referencePulse + pulses + 1)};
    }
    return retVal;
}
