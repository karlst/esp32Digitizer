/* Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
 * Timestamp-derived pulse numbering, checked against the separate pulse counter.
 */
#pragma once
#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "driver/pulse_cnt.h"
#include "CaptureStats.h"
#include "FsyncAssociationAnalysis.h"
class PpsSampleTest;

/**
 * Core 0 owns this experiment. Each poll reads a recent FSYNC timestamp and
 * advances a calculated pulse number by rounding the timestamp difference to
 * whole sample periods. PCNT supplies the initial number and later comparisons
 * only; it never repairs the calculated number. After shutdown, verify the
 * prediction against the settled counter and timestamp, then map saved anchors
 * to actual retained words using startup's pulse 2 -> sample 0 convention.
 * Core 1's words and decoder statistics are read only after decoding drains.
 * Optional PpsSampleTest receives numbered observations; this class does not
 * capture PPS itself. No disk operations occur.
 */
class FsyncIndexTest
{
public:
    void reset(PpsSampleTest* ppsSampleTest = nullptr);
    void measure(pcnt_unit_handle_t counter);
    bool getFailed() const;
    bool report(uint32_t run, bool clean, uint32_t finalPulse, uint32_t finalTicks,
        const uint32_t* words, uint32_t storedWords, const CaptureStats& stats) const;

private:
    PpsSampleTest* _ppsSampleTest = nullptr;
    // First eight disagreements preserve evidence without printing during capture.
    struct Example
    {
        uint32_t _observation;
        uint32_t _calculated;
        int _observed;
        uint32_t _ticks;
        int _error;
    };
    Example _examples[8] = {};
    unsigned _exampleCount = 0;
    uint32_t _observations = 0, _initialSkipped = 0;
    uint32_t _checked = 0, _matched = 0, _mismatched = 0;
    uint32_t _movingReference = 0, _slowReference = 0, _staleReference = 0;
    uint32_t _unsafeSteps = 0, _maxGapTicks = 0;
    uint32_t _firstPulse = 0, _calculatedPulse = 0, _previousTicks = 0;
    uint32_t _firstTicks = 0;
    // First 64 observations can map into the retained 65536-word sample array.
    FsyncAssociationAnalysis::Anchor _sampleAnchors[64] = {};
    unsigned _sampleAnchorCount = 0;
    uint64_t _advancedPulses = 0;
    double _maxResidualSamples = 0;
    int64_t _previousAt = 0;
    uint32_t _random = 0x68a5de13;
    bool _initialized = false, _lost = false;
    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
};
