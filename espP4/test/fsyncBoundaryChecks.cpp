// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// Check the actual boundary arithmetic without pretending to read hardware.
#include "FsyncBoundaryMath.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

/* **
 * main - Test retry after skipped edges, wrapped ticks and ambiguous anchors.
 * Valid cases retain the supplied hardware counts exactly; invalid cases must
 * not replace the caller's last valid output with a guessed timestamp.
 */
int main()
{
    // Reproduce the board's four-period jump (625 ticks). The same rolling
    // comparison used by the hardware loop must reject it, keep the newer
    // capture, tolerate an unchanged register, then accept the next edge.
    uint32_t before = 447602980;
    uint32_t after = before;
    assert(!FsyncBoundaryMath::advance(before, after, 447603605));
    assert(before == 447602980 && after == 447603605);
    assert(!FsyncBoundaryMath::advance(before, after, 447603605));
    assert(FsyncBoundaryMath::advance(before, after, 447603761));
    assert(before == 447603605 && after == 447603761);
    FsyncBoundary retriedBoundary;
    assert(FsyncBoundaryMath::resolve(2855298, 447596886, before, after, retriedBoundary));
    assert(retriedBoundary._pulseAfter == 2855342);
    assert(retriedBoundary._tcBefore == 447603605 && retriedBoundary._tcAfter == 447603761);

    // Several consecutive skipped edges must not become a false success.
    // The final actual pair also remains valid across a raw timer wrap.
    after = 0xfffffd7f;
    assert(!FsyncBoundaryMath::advance(before, after, 0xfffffeb8));
    assert(!FsyncBoundaryMath::advance(before, after, 0xfffffff0));
    assert(FsyncBoundaryMath::advance(before, after, 0x8d));
    assert(before == 0xfffffff0 && after == 0x8d);

    FsyncBoundary boundary;
    assert(FsyncBoundaryMath::resolve(2, 1000, 1000, 1156, boundary));
    assert(boundary._pulseAfter == 3 && boundary._tcBefore == 1000 && boundary._tcAfter == 1156);
    assert(FsyncBoundaryMath::resolve(100, 0xfffffff0, 0xfffffff0, 0x8d, boundary));
    assert(boundary._pulseAfter == 101 && boundary._tcBefore == 0xfffffff0 && boundary._tcAfter == 0x8d);
    assert(FsyncBoundaryMath::resolve(100, 1000, 161000, 161157, boundary));
    assert(boundary._pulseAfter == 1125);
    for (uint32_t spacing : {0U, 155U, 158U, 312U, 313U})
    {
        assert(!FsyncBoundaryMath::resolve(100, 1000, 161000, 161000 + spacing, boundary));
        assert(boundary._pulseAfter == 1125);
    }
    assert(!FsyncBoundaryMath::resolve(100, 1000, 1610080, 1610236, boundary));
    assert(!FsyncBoundaryMath::resolve(100, 1000, 1080, 1236, boundary));
    assert(!FsyncBoundaryMath::resolve(100, 1000, 999, 1156, boundary));
    assert(!FsyncBoundaryMath::resolve(0xffffffff, 1000, 1000, 1156, boundary));
    assert(!FsyncBoundaryMath::resolve(1, 1000, 1000, 1156, boundary));
    std::puts("PASS: observed adjacent FSYNC numbering, raw wrap and ambiguity guards");
    return 0;
}
