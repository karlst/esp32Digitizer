/* Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
 * Combine the existing timing diagnostic and SD destination without duplicating
 * reception, decoding, sample packing, buffering, or disk-writing code.
 */
#pragma once
#include "FsyncAssociationTest.h"
#include "SdCaptureDestination.h"

/**
 * Default commands use the original RAM diagnostic. Explicit d selection enables
 * the combined PPS/SD experiment until reset. Core 0 prepares storage first,
 * then arms timing hardware, before allowing ADC start. Core 1 submits each
 * decoded batch to the ordinary SD adapter and timing diagnostic. The mode and
 * references are fixed before Core 1 can submit. poll runs from the startup loop;
 * it never writes files itself. The recorder's existing Core 0 task owns SD I/O.
 */
class FsyncSdTest : public CaptureDestination, public RecordingConsoleCommands
{
public:
    FsyncSdTest(FsyncAssociationTest& fsyncAssociationTest, SdRecorder& sdRecorder,
        SdCaptureDestination& sdCaptureDestination);
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
    FsyncAssociationTest& _fsyncAssociationTest;
    SdRecorder& _sdRecorder;
    SdCaptureDestination& _sdCaptureDestination;
    Ads127Config _config = ads127DefaultConfig;
    uint32_t _run = 0;
    bool _enabled = false, _initialized = false, _preparing = false;
    bool _hardwarePrepared = false, _failed = false;
};
