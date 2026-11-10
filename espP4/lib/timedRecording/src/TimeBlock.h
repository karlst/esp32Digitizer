// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// The timing record preceding one interval's packed ADC samples.
#pragma once
#include <cstdint>

/**
 * First/last sample FSYNC counts and consecutive PPS counts share one extended
 * hardware timer origin. They are integer ticks, not nanoseconds. sCount is
 * the number of three-byte samples immediately following this record. nSec
 * counts seconds from the file header's UTC origin, beginning at zero.
 * These are values, not a packed disk representation; use TimedFileFormat.
 * _tcPpsFirst/_tcPpsLast are called tcPpsCurrent/tcPpsNext in the client document:
 * the opening and closing PPS of this interval. The last sample may occur just
 * after the closing PPS because software chooses the boundary shortly afterward.
 * Sample interpolation uses (tcFsLast-tcFsFirst)/(sCount-1), whereas measured
 * ticks per second use tcPpsLast-tcPpsFirst. Neither is a fixed nominal clock rate.
 */
struct TimeBlock
{
    uint64_t _tcFsFirst = 0, _tcFsLast = 0;
    uint64_t _tcPpsFirst = 0, _tcPpsLast = 0;
    uint32_t _sCount = 0;
    uint16_t _nSec = 0;
};
