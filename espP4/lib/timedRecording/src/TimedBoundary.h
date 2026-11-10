// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// One PPS and the two adjacent samples selected shortly after that PPS.
#pragma once
#include <cstdint>

/**
 * One PPS and the two consecutive FSYNC edges chosen shortly afterward.
 * All times are 64-bit extended timer ticks, not nanoseconds. _tcFsBefore is
 * the last sample of the closing block; _tcFsAfter is the first of the opening
 * block. _sampleFirst is that later sample's zero-based acquisition-wide index,
 * not a file-relative index. For adjacent boundaries A and B, the interval has
 * B._sampleFirst - A._sampleFirst samples, from A._tcFsAfter to B._tcFsBefore.
 * The timing task copies this value into the formatter's queue; it contains no
 * pointer to live capture registers. PPS need not coincide with either FSYNC.
 */
struct TimedBoundary
{
    uint64_t _tcPps = 0, _tcFsBefore = 0, _tcFsAfter = 0;
    uint64_t _sampleFirst = 0;
};
