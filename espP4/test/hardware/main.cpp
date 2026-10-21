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
 * @brief P4 test startup; reuse the real application with bench-only services.
 * Objects remain alive until reset because tasks and callbacks borrow them.
 */
#include "RecordingApplication.h"
#include "AdcReceiver.h"
#include "SampleDecoder.h"
#include "RecordingController.h"
#include "ConsoleRecordingReporter.h"
#include "SdCaptureDestination.h"
#include "RecordingHarness.h"
#include "test/support/ads127BenchTiming.h"
static ConsoleRecordingReporter consoleRecordingReporter;
#if CONFIG_P4_HARDWARE_SMOKE
#include "SdSmokeSummary.h"
static constexpr uint64_t reservedBytes = 128ULL * 1024 * 1024;
static_assert(reservedBytes > 512ULL + 1126400ULL * 3 *
    ads127BenchTiming::receiverTimeoutMs / 1000,
    "Reserve must cover the receiver timeout with ten percent sample-rate margin");
static SdSmokeSummary sdSmokeSummary;
static SdRecorder sdRecorder(reservedBytes, &sdSmokeSummary, &consoleRecordingReporter);
#else
static SdRecorder sdRecorder(10000000000ULL, nullptr, &consoleRecordingReporter);
#endif
static SdCaptureDestination sdCaptureDestination(sdRecorder);
static RecordingHarness recordingHarness(sdRecorder);
static AdcReceiver adcReceiver;
static SampleDecoder sampleDecoder(adcReceiver, &recordingHarness);
static RecordingApplication recordingApplication(adcReceiver, sampleDecoder, sdCaptureDestination,
    ads127BenchTiming::receiverTimeoutMs, &recordingHarness);
static RecordingController recordingController(recordingApplication, consoleRecordingReporter, &recordingHarness);

/** @brief Initialize services once, then run the same command loop as ordinary recording. */
extern "C" void app_main()
{
    ESP_ERROR_CHECK(sdRecorder.initTask() ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(recordingHarness.initialize() ? ESP_OK : ESP_ERR_NO_MEM);
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
