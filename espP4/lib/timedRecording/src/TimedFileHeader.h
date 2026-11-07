// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// Version-one file metadata. Encode fields explicitly; never write this struct.
#pragma once
#include <cstdint>

/**
 * Calendar origin for one file. utcFirst is the UTC second belonging to its
 * first tBlock's opening PPS, not the time the file was allocated or opened.
 * Test sources may supply an explicitly synthetic origin. delayNs converts
 * output FSYNC time to analog-input time. pairsPerFile is the configured limit;
 * the all-zero tBlock marks actual normal completion, including a shorter file.
 */
struct TimedFileHeader
{
    uint64_t _utcFirst = 0;
    int32_t _delayNs = 2553;
    uint32_t _pairsPerFile = 60;
};
