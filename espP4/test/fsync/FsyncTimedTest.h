// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// Explicit timed-format bench mode around the unchanged acquisition services.
#pragma once
#include "FsyncSdTest.h"
#include "TimedRecorder.h"

/**
 * Command t selects the new format. Other commands delegate to FsyncSdTest,
 * preserving the passing sample-only d experiment and RAM diagnostics. Core 0
 * prepares storage before arming timing; Core 1 supplies the same decoded words
 * to the small timing diagnostic window and TimedRecorder's staging queue.
 * Reset is required between destination modes so no two SD mounts/tasks run.
 */
class FsyncTimedTest : public CaptureDestination, public RecordingConsoleCommands
{
public:
    FsyncTimedTest(FsyncAssociationTest& timing, FsyncSdTest& previous, TimedRecorder& recorder);
    bool prepare(uint32_t run, const Ads127Config& config) override;
    bool getReady() const override;
    void cancelPreparation() override;
    bool submit(const uint32_t* words, size_t count) override;
    bool getFailed() const override;
    bool getFull() const override;
    void finish(uint32_t run, uint32_t rate, bool clean, uint32_t samples) override;
    void poll();
    void command(RecordingApplication& application, char key) override;
private:
    FsyncAssociationTest& _timing;
    FsyncSdTest& _previous;
    TimedRecorder& _recorder;
    bool _selected = false, _otherSelected = false, _preparing = false, _prepared = false, _failed = false;
    unsigned _nextBoundary = 0;
    uint32_t _run = 0;
    Ads127Config _config = ads127DefaultConfig;
};
