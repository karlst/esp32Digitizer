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
 */
class FsyncBoundaryCapture
{
public:
    /** Raw evidence retained even when capture rejects the pair. Core 0 owns it. */
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
