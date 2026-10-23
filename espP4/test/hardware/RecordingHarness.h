/*
 * Copyright (C) 2026 Karl Stock
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of ESP32 Digitizer.
 * You may redistribute and/or modify it under the GNU General Public License
 * as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 *
 * Distributed WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License in the repository LICENSE file or <https://www.gnu.org/licenses/>.
 */

/**
 * @file RecordingHarness.h
 * Own P4 bench-only objects and connect them to the ordinary recording application.
 */
#pragma once
#include "RecordingObserver.h"
#include "RecordingConsoleCommands.h"
#include "RamCaptureDestination.h"
#include "SdWriteTest.h"
#include "DecoderTiming.h"
#include "sdkconfig.h"
#if CONFIG_P4_CAPTURE_PROFILE
#include "CaptureProfile.h"
#endif

/**
 * Supply test commands, RAM summaries and optional runtime measurements.
 * Core 0 calls command/start/stop; Core 1 updates decoder timings during capture.
 * The application drains Core 1 before calling stopped, so reporting has no race.
 * Production startup never includes this class or allocates its test buffers/tasks.
 */
class RecordingHarness : public RecordingObserver, public RecordingConsoleCommands
{
public:
    explicit RecordingHarness(SdRecorder& sdRecorder);
    bool initialize();
    bool line(RecordingApplication& recordingApplication, const char* text) override;
    void command(RecordingApplication& recordingApplication, char key) override;
    void started(TaskHandle_t control, TaskHandle_t decoder) override;
    void decoded(int64_t waitUs, int64_t decodeUs, uint32_t bytes) override;
    void paused(int64_t elapsedUs) override;
    void poll() override;
    void stopping() override;
    void stopped(uint32_t rate, uint32_t slots, int64_t gapUs, int64_t copyUs) override;

private:
    SdRecorder& _sdRecorder;
    SdWriteTest _sdWriteTest;
    RamCaptureDestination _ramCaptureDestination;
    DecoderTiming _decoderTiming;
    bool _ramMode = false;
    uint32_t _seconds = 30;
#if CONFIG_P4_CAPTURE_PROFILE
    CaptureProfile _captureProfile;
#endif
};
