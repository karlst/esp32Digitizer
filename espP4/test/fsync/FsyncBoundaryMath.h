// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// Hardware-independent arithmetic used by consecutive-FSYNC capture and tests.
#pragma once
#include "FsyncBoundary.h"

/**
 * Resolve a directly read adjacent pair against a previously numbered capture.
 * This does not read hardware or establish the initial numbering reference.
 * Nominal ratio and rounding margin match FsyncIndexTest; no nominal timestamp
 * is substituted for either observed edge.
 */
class FsyncBoundaryMath
{
public:
    /* **
     * advance - Keep the latest two actual register reads and report whether
     * they are consecutive FSYNCs. The capture loop calls this after each read.
     * Repeated values and skipped edges return false; the newer value still
     * becomes the starting point for the next attempt. Unsigned subtraction
     * handles the 32-bit timer wrapping. No timestamp is estimated.
     */
    static bool advance(uint32_t& beforeTicks, uint32_t& afterTicks, uint32_t nextTicks)
    {
        beforeTicks = afterTicks;
        afterTicks = nextTicks;
        const uint32_t spacing = afterTicks - beforeTicks;
        const bool retVal = spacing == 156 || spacing == 157;
        return retVal;
    }

    static bool resolve(uint32_t referencePulse, uint32_t referenceTicks,
        uint32_t beforeTicks, uint32_t afterTicks, FsyncBoundary& boundary);
};
