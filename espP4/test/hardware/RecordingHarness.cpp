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
 * @file RecordingHarness.cpp
 * Bench commands and diagnostics around the unchanged recording data path.
 */
#include "RecordingHarness.h"
#include "RecordingApplication.h"
#include <cstdio>
#include <cstring>
#if CONFIG_P4_HARDWARE_SMOKE
#include "SdSmokeSummary.h"
#endif

/* **
 * RecordingHarness - Borrow the startup-owned recorder; construction starts no
 * test. Smoke startup also supplies its SD readback reporter; other bench
 * builds leave that pointer null and cannot select the grounded-input report.
 */
RecordingHarness::RecordingHarness(SdRecorder& sdRecorder, SdSmokeSummary* sdSmokeSummary)
    : _sdRecorder(sdRecorder), _sdSmokeSummary(sdSmokeSummary)
{
}

/* **
 * initialize - Create the generated-data task, which waits until an explicit t/u
 * command.
 */
bool RecordingHarness::initialize()
{
    printf("Bench keys: w=RAM before other commands; t/u=SD write test; s=stop SD test.\n");
    return _sdWriteTest.initialize(_sdRecorder);
}

/* **
 * line - Handle hardware-smoke configuration before recording preparation.
 * @NOISE 1 enables individual-sample reporting; @NOISE 0 disables it. Accept
 * either only while idle. @TEST also clears that selection, so a later ordinary
 * test cannot accidentally request the lengthy noise transfer.
 * Accept 30, 300 or 600 seconds while idle, with SD selected.
 *
 * A 30-second run uses two 16-second files and stops when full. Longer runs use
 * five 16-second files and overwrite the oldest selected file. Add these files
 * without erasing earlier recordings. Return true for a recognized @TEST or
 * @NOISE command, including a rejected request; the printed reply says whether its
 * settings were accepted. This does not start capture.
 */
bool RecordingHarness::line(RecordingApplication& recordingApplication, const char* text)
{
    bool retVal = false;
#if CONFIG_P4_HARDWARE_SMOKE
    if (strncmp(text, "@NOISE", 6) == 0)
    {
        retVal = true;
        unsigned enabled = 0;
        char extra = 0;
        const bool accepted = sscanf(text, "@NOISE %u %c", &enabled, &extra) == 1 && enabled <= 1 &&
            recordingApplication.getIdle() && !recordingApplication.isRecordingDestinationLocked() &&
            !_ramMode && _sdSmokeSummary != nullptr;
        if (accepted)
        {
            _sdSmokeSummary->setNoiseReporting(enabled != 0);
            printf("NOISE OK enabled=%u\n", enabled);
        }
        else
        {
            printf("NOISE rejected\n");
        }
    }
    else if (strncmp(text, "@TEST", 5) == 0)
    {
        retVal = true;
        unsigned seconds = 0;
        char extra = 0;
        bool accepted = sscanf(text, "@TEST %u %c", &seconds, &extra) == 1 &&
                        (seconds == 30 || seconds == 300 || seconds == 600) &&
                        recordingApplication.getIdle() && !recordingApplication.isRecordingDestinationLocked() && !_ramMode;
        if (accepted)
        {
            RecordingSessionConfig recordingSessionConfig;
            recordingSessionConfig._preparation = RecordingSessionConfig::Preparation::addFiles;
            recordingSessionConfig._secondsPerFile = 16;
            recordingSessionConfig._fileCount = seconds == 30 ? 2 : 5;
            recordingSessionConfig._fullPolicy = seconds == 30 ? RecordingSessionConfig::FullPolicy::stopWhenFull : RecordingSessionConfig::FullPolicy::overwriteOldest;
            accepted = _sdRecorder.configure(recordingSessionConfig) && recordingApplication.setTimeout((seconds + 5) * 1000);
        }
        if (accepted)
        {
            _seconds = seconds;
            if (_sdSmokeSummary != nullptr)
            {
                _sdSmokeSummary->setNoiseReporting(false);
            }
            printf("TEST OK seconds=%u\n", seconds);
        }
        else
        {
            printf("TEST rejected\n");
        }
    }
#else
    (void)recordingApplication;
    (void)text;
#endif
    return retVal;
}

/* **
 * command - Handle bench-only keys on Core 0, refusing SD tests in RAM mode.
 */
void RecordingHarness::command(RecordingApplication& recordingApplication, char key)
{
#if CONFIG_P4_HARDWARE_SMOKE
    if (key == 'h')
    {
        printf("SMOKE_P4 version=9 ready=%u seconds=%lu\n",
            unsigned(recordingApplication.getIdle() && !recordingApplication.isRecordingDestinationLocked() && !_ramMode),
            static_cast<unsigned long>(_seconds));
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
        else
        {
            printf("Mode already selected; reset P4 before changing modes.\n");
        }
    }
    else if (_ramMode && (key == 't' || key == 'u' || key == 's'))
    {
        printf("RAM mode: SD commands disabled until reset.\n");
    }
    else if (key == 't' || key == 'u')
    {
        const bool accepted = recordingApplication.lockRecordingDestination() && _sdWriteTest.start(key == 't' ? 812500 : 1024000);
        if (!accepted)
        {
            printf("SD TEST refused: recording or storage busy\n");
        }
    }
    else if (key == 's')
    {
        _sdWriteTest.stop();
    }
}

/* **
 * started - Reset timings before ADC start and optionally begin task-counter
 * measurements.
 */
void RecordingHarness::started(TaskHandle_t control, TaskHandle_t decoder)
{
    (void)control;
    (void)decoder;
    _decoderTiming = {};
#if CONFIG_P4_CAPTURE_PROFILE
    _captureProfile.begin(control, decoder, _sdRecorder.getTaskHandle());
#endif
}

/* **
 * decoded - Core 1 records queue wait and decode durations for one raw block.
 */
void RecordingHarness::decoded(int64_t waitUs, int64_t decodeUs, uint32_t bytes)
{
    _decoderTiming.recordWait(waitUs);
    _decoderTiming.recordDecode(decodeUs, bytes);
}

/* **
 * paused - Core 1 records a completed scheduling pause, without adding another
 * delay.
 */
void RecordingHarness::paused(int64_t elapsedUs)
{
    _decoderTiming.recordPause(elapsedUs);
}

/* **
 * poll - Core 0 samples optional CPU counters while capture is running.
 */
void RecordingHarness::poll()
{
#if CONFIG_P4_CAPTURE_PROFILE
    _captureProfile.poll();
#endif
}

/* **
 * stopping - Freeze profiling before draining so file verification is excluded.
 */
void RecordingHarness::stopping()
{
#if CONFIG_P4_CAPTURE_PROFILE
    _captureProfile.finish();
#endif
}

/* **
 * stopped - Core 0 prints measurements after Core 1 has returned every queued
 * block.
 */
void RecordingHarness::stopped(uint32_t rate, uint32_t slots, int64_t gapUs, int64_t copyUs)
{
    _decoderTiming.report(rate, slots, gapUs, copyUs);
#if CONFIG_P4_CAPTURE_PROFILE
    _captureProfile.report();
#endif
}
