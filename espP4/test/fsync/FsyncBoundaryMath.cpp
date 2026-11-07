// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// Pure integer validation; use the same implementation on the PC and P4.
#include "FsyncBoundaryMath.h"
#include <limits>

/* **
 * resolve - Accept only consecutive captures 156 or 157 timer ticks apart.
 * At 160 MHz / 1.024 MSPS, one sample is nominally 156.25 ticks; observing a
 * two-sample jump cannot pass this guard. Number the earlier edge relative to
 * the supplied anchor, requiring a gap <=10 ms and <0.25-sample residual.
 * Unsigned subtraction handles one raw timer wrap. Preserve output on failure.
 */
bool FsyncBoundaryMath::resolve(uint32_t referencePulse, uint32_t referenceTicks,
    uint32_t beforeTicks, uint32_t afterTicks, FsyncBoundary& boundary)
{
    const uint32_t spacing = afterTicks - beforeTicks;
    const uint32_t elapsed = beforeTicks - referenceTicks;
    const uint64_t scaled = uint64_t(elapsed) * 4;
    const uint64_t pulses = (scaled + 312) / 625;
    const uint64_t rounded = pulses * 625;
    const uint64_t error = scaled > rounded ? scaled - rounded : rounded - scaled;
    const bool retVal = referencePulse >= 2 && elapsed <= 1600000 &&
        (spacing == 156 || spacing == 157) && error <= 156 &&
        pulses + referencePulse + 1 <= std::numeric_limits<uint32_t>::max();
    if (retVal)
    {
        boundary = {beforeTicks, afterTicks, uint32_t(referencePulse + pulses + 1)};
    }
    return retVal;
}
