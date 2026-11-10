// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// Bounded reading of two consecutive values from FSYNC's capture register.
#pragma once
#include "FsyncBoundary.h"
#include "freertos/FreeRTOS.h"

/**
 * Core 0 calls once after each mapped PPS. Hardware already captures every
 * FSYNC into MCPWM group 0/channel 0. A short bounded read loop observes its
 * changes until two adjacent captures are read. A skipped edge moves the search
 * forward; it does not install an interrupt at the 1.024-MHz sample rate.
 * Failure is reported rather than guessing the missing predecessor timestamp.
 * This is the once-per-PPS search, not FsyncIndexTest's millisecond four-read
 * observation. It reads only the FSYNC capture register; PCNT is not read here.
 */
class FsyncBoundaryCapture
{
public:
    /**
     * Last two raw hardware tick counts and CPU cycles spent searching, retained
     * even on failure. _numbered means the pair passed resolve's arithmetic;
     * capture also requires the search to finish within its elapsed-time limit.
     * Core 0 owns this record and reports it only after acquisition stops.
     */
    struct ReadResult
    {
        uint32_t _tcBefore = 0;
        uint32_t _tcAfter = 0;
        uint32_t _elapsedCycles = 0;
        bool _numbered = false;
    };
    bool capture(uint32_t referencePulse, uint32_t referenceTicks,
        FsyncBoundary& boundary, ReadResult& readResult);
private:
    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
};
