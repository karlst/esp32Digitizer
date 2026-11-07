/* Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
 * Arithmetic checks shared by the hardware diagnostic and PC regression tests.
 */
#pragma once
#include <cstddef>
#include <cstdint>

/**
 * Check saved FSYNC pulse numbers against their hardware capture times.
 * This class has no device access. Passing establishes consistency of the
 * supplied observations, not that startup or physical wiring was correct.
 */
class FsyncAssociationAnalysis
{
public:
    /**
     * One matched hardware pulse count and 32-bit capture-timer reading.
     */
    struct Anchor
    {
        // One-based since arming; the deliberately discarded frame is pulse 1.
        uint32_t _pulseCount = 0;
        // Free-running 32-bit timer, nominally 6.25 ns per tick; may wrap.
        uint32_t _ticks = 0;
    };

    /**
     * Arithmetic verdict and diagnostics; matched counts retained word/time pairs.
     */
    struct Result
    {
        bool _passed = false;
        double _ticksPerSample = 0;
        double _maxResidualTicks = 0;
        // Only this consecutive range in the supplied anchor array has words
        // retained in RAM. Earlier startup and later shutdown pulses are excluded.
        size_t _matched = 0;
        size_t _firstMatched = 0;
    };

    static Result analyze(const Anchor* anchors, size_t count, uint32_t words, uint32_t discardedFrames = 0);
    static uint32_t extendCount(uint32_t baseline, int hardwareCount);
};
