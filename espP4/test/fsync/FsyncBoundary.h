// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// Two actually captured consecutive FSYNC edges, numbered from a checked anchor.
#pragma once
#include <cstdint>

/**
 * before is the last sample of the closing sBlock; after is the first sample
 * of the opening sBlock. These raw 32-bit timer counts can straddle a wrap.
 * pulseAfter is a pulse number, not a sample index; sample index is pulseAfter-2
 * under the diagnostic's established startup convention.
 */
struct FsyncBoundary
{
    uint32_t _tcBefore = 0, _tcAfter = 0, _pulseAfter = 0;
};
