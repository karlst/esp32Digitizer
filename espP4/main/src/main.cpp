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
 * @file main.cpp
 * Ordinary P4 recording startup: construct services and run the command loop.
 * ESP-IDF calls app_main on Core 0 after boot. Static lifetime is required because
 * the recording and decoder tasks retain pointers to these objects until reset.
 * No test generator, waveform summary or profiler is constructed in this build.
 */
#include "RecordingApplication.h"
#include "AdcReceiver.h"
#include "SampleDecoder.h"
#include "RecordingController.h"
#include "ConsoleRecordingReporter.h"
#include "SdCaptureDestination.h"


static ConsoleRecordingReporter consoleRecordingReporter;

/* **
 * getRecordingSessionConfig - Preserve old recordings and reserve eleven
 * five-minute files for this run.
 */
static RecordingSessionConfig getRecordingSessionConfig()
{
    RecordingSessionConfig recordingSessionConfig;
    recordingSessionConfig._preparation = RecordingSessionConfig::Preparation::addFiles;
    recordingSessionConfig._fullPolicy = RecordingSessionConfig::FullPolicy::stopWhenFull;
    recordingSessionConfig._secondsPerFile = 300;
    recordingSessionConfig._fileCount = 11;
    return recordingSessionConfig;
}

static SdRecorder sdRecorder(getRecordingSessionConfig(), nullptr, &consoleRecordingReporter);
static SdCaptureDestination sdCaptureDestination(sdRecorder);
static AdcReceiver adcReceiver;
static SampleDecoder sampleDecoder(adcReceiver);
static RecordingApplication recordingApplication(adcReceiver, sampleDecoder, sdCaptureDestination, 1805000);
static RecordingController recordingController(recordingApplication, consoleRecordingReporter);

/* **
 * app_main - Allocate recording resources, then service commands; no capture
 * starts at boot.
 */
extern "C" void app_main()
{
    ESP_ERROR_CHECK(sdRecorder.initTask() ? ESP_OK : ESP_ERR_NO_MEM);
    recordingApplication.initialize();
    recordingController.initialize();

    // Core 0 services requests and supervises capture. Data transfer runs in
    // the receiver interrupt, Core 1 decoder task and Core 0 SD task.
    for (;;)
    {
        recordingController.poll();
        recordingApplication.poll();
        // Request a 1 ms scheduler delay, rounded to the configured OS tick.
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
