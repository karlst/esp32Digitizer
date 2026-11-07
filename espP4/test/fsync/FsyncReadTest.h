/* Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
 * Statistics for the optional FSYNC register-read experiment.
 */
#pragma once
#include <cstdint>
#include "freertos/FreeRTOS.h"

/**
 * Core 0 calls reset before acquisition, measure during acquisition, and report
 * after shutdown. No pointers or statistics cross cores. This observes PCNT
 * unit 0 and MCPWM capture channel 0, allocated by FsyncAssociationTest.
 * Consistency means both fields changed or neither did; it is not independent
 * proof that the two hardware values describe the same physical edge.
 */
class FsyncReadTest
{
public:
    void reset();
    void measure();
    void report(uint32_t run, bool clean) const;

private:
    void measureReadTimes();

    // Eight separate timing brackets, each observed once per application poll.
    // Index order and labels are identical in measureReadTimes and report.
    uint32_t _readMin[8] = {};
    uint32_t _readMax[8] = {};
    uint64_t _readTotal[8] = {};
    uint32_t _readTrials = 0;
    // Rows distinguish LOW at both ends, HIGH at both ends, and changed level.
    uint32_t _attempts[3] = {};
    uint32_t _inconsistent[3] = {};
    uint32_t _slow[3] = {};
    uint32_t _rejected[3] = {};
    uint32_t _sets = 0;
    uint32_t _exhausted = 0;
    uint32_t _allInconsistent = 0;
    uint32_t _maxAttempts = 0;
    uint32_t _same = 0;
    uint32_t _bothChanged = 0;
    uint32_t _comparisonMin = UINT32_MAX, _comparisonMax = 0;
    uint32_t _setMin = UINT32_MAX, _setMax = 0;
    uint64_t _comparisonTotal = 0, _setTotal = 0;
    uint32_t _overhead = UINT32_MAX;
    uint32_t _random = 0x91e10da5;
    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
};
