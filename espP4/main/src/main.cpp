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

/** @file main.cpp
 * @brief Ordinary P4 recording startup: construct services and run the command loop.
 * ESP-IDF calls app_main on Core 0 after boot. Static lifetime is required because
 * the recording and decoder tasks retain pointers to these objects until reset.
 * No test generator, waveform summary or profiler is constructed in this build.
 */
#include "RecordingApplication.h"
#include "SdCaptureDestination.h"
static SdRecorder sdRecorder;
static SdCaptureDestination sdCaptureDestination(sdRecorder);
static RecordingApplication recordingApplication(sdCaptureDestination, 1805000);
/** @brief Allocate recording resources, then service commands; no capture starts at boot. */
extern "C" void app_main()
{
    ESP_ERROR_CHECK(sdRecorder.initialize() ? ESP_OK : ESP_ERR_NO_MEM);
    recordingApplication.initialize();
    for (;;) { recordingApplication.poll(); }
}
