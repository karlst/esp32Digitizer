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

/** @file RecordingHarness.cpp
 * @brief Bench commands and diagnostics around the unchanged recording data path.
 */
#include "RecordingHarness.h"
#include "RecordingApplication.h"
#include <cstdio>

/** @brief Borrow the startup-owned recorder; construction starts no test. */
RecordingHarness::RecordingHarness(SdRecorder& sdRecorder) : _sdRecorder(sdRecorder) {}

/** @brief Create the generated-data task, which waits until an explicit t/u command. */
bool RecordingHarness::initialize()
{
    printf("Bench keys: w=RAM before other commands; t/u=SD write test; s=stop SD test.\n");
    return _sdWriteTest.initialize(_sdRecorder);
}

/** @brief Handle bench-only keys on Core 0, refusing SD tests in RAM mode. */
void RecordingHarness::command(RecordingApplication& recordingApplication, char key)
{
#if CONFIG_P4_HARDWARE_SMOKE
    if (key == 'h')
    {
        printf("SMOKE_P4 version=3 ready=%u seconds=30\n",
               unsigned(recordingApplication.getIdle() && !recordingApplication.isRecordingDestinationLocked() && !_ramMode));
    }
    else
#endif
    if (key == 'w')
    {
        if (recordingApplication.selectDestination(_ramCaptureDestination, 5000))
        {
            _ramMode = true;
            printf("RAM READY: no card access; use Feather DAC firmware with RAMFULL support, then 6.\n");
        }
        else { printf("Mode already selected; reset P4 before changing modes.\n"); }
    }
    else if (_ramMode && (key == 't' || key == 'u' || key == 's'))
    { printf("RAM mode: SD commands disabled until reset.\n"); }
    else if (key == 't' || key == 'u')
    {
        const bool accepted = recordingApplication.lockRecordingDestination() && _sdWriteTest.start(key == 't' ? 812500 : 1024000);
        if (!accepted) { printf("SD TEST refused: recording or storage busy\n"); }
    }
    else if (key == 's') { _sdWriteTest.stop(); }
}

/** @brief Reset timings before ADC start and optionally begin task-counter measurements. */
void RecordingHarness::started(TaskHandle_t control, TaskHandle_t decoder)
{
    (void)control;
    (void)decoder;
    _decoderTiming = {};
#if CONFIG_P4_CAPTURE_PROFILE
    _captureProfile.begin(control, decoder, _sdRecorder.getTaskHandle());
#endif
}

/** @brief Core 1 records queue wait and decode durations for one raw block. */
void RecordingHarness::decoded(int64_t waitUs, int64_t decodeUs, uint32_t bytes)
{ _decoderTiming.recordWait(waitUs); _decoderTiming.recordDecode(decodeUs, bytes); }

/** @brief Core 1 records a completed scheduling pause, without adding another delay. */
void RecordingHarness::paused(int64_t elapsedUs) { _decoderTiming.recordPause(elapsedUs); }

/** @brief Core 0 samples optional CPU counters while capture is running. */
void RecordingHarness::poll()
{
#if CONFIG_P4_CAPTURE_PROFILE
    _captureProfile.poll();
#endif
}

/** @brief Freeze profiling before draining so file verification is excluded. */
void RecordingHarness::stopping()
{
#if CONFIG_P4_CAPTURE_PROFILE
    _captureProfile.finish();
#endif
}

/** @brief Core 0 prints measurements after Core 1 has returned every queued block. */
void RecordingHarness::stopped(uint32_t rate, uint32_t slots, int64_t gapUs, int64_t copyUs)
{
    _decoderTiming.report(rate, slots, gapUs, copyUs);
#if CONFIG_P4_CAPTURE_PROFILE
    _captureProfile.report();
#endif
}
