// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// The timing record preceding one interval's packed ADC samples.
#pragma once
#include <cstdint>

/**
 * First/last sample FSYNC counts and bounding PPS counts share one extended
 * hardware timer origin. They are integer ticks, not nanoseconds. sCount is
 * the number of three-byte samples immediately following this record. nSec
 * counts seconds from the file header's UTC origin, beginning at zero.
 * These are values, not a packed disk representation; use TimedFileFormat.
 */
struct TimeBlock
{
    uint64_t _tcFsFirst = 0, _tcFsLast = 0;
    uint64_t _tcPpsFirst = 0, _tcPpsLast = 0;
    uint32_t _sCount = 0;
    uint16_t _nSec = 0;
};
