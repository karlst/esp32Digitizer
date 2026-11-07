/* Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
 * Use ordinary receiver reports with an accurate diagnostic startup description.
 */
#pragma once
#include "ConsoleRecordingReporter.h"

/**
 * Keep ordinary acquisition/storage reports. Startup describes the safe default;
 * SD files are created only after explicit combined-test selection.
 */
class FsyncConsoleReporter : public ConsoleRecordingReporter
{
public:
    /* **
     * startup - Controller calls once after opening command ports; no run starts.
     */
    void startup() const override
    {
        message("FSYNC diagnostic: RAM default; SD requires explicit PPS/SD mode. DCLK=0 FSYNC=1 DOUT0=2 RX=4 TX=5.\n");
    }
};
