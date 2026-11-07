// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// One PPS and the two adjacent samples selected shortly after that PPS.
#pragma once
#include <cstdint>

/** All times are extended ticks; sampleFirst is the next block's absolute index. */
struct TimedBoundary
{
    uint64_t _tcPps = 0, _tcFsBefore = 0, _tcFsAfter = 0;
    uint64_t _sampleFirst = 0;
};
