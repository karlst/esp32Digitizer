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

/** @file RecordingController.h
 * @brief Today's console/Feather controller; replace this when the command source changes.
 */
#pragma once
#include "RecordingApplication.h"
#include "RecordingReporter.h"
#include "RecordingConsoleCommands.h"
#include "BenchLine.h"
#include "driver/uart.h"
#include "freertos/queue.h"

/**
 * @brief Translate console keys and Feather messages into recording operations.
 * Startup initializes and polls this on Core 0. The application knows none of
 * this protocol. Required READY3/STOP/error replies remain enabled regardless
 * of optional terminal reporting. All borrowed objects must live until reset.
 */
class RecordingController : public RecordingEvents
{
public:
    RecordingController(RecordingApplication& recordingApplication, RecordingReporter& recordingReporter,
        RecordingConsoleCommands* recordingConsoleCommands = nullptr);
    RecordingController(const RecordingController&) = delete;
    RecordingController& operator=(const RecordingController&) = delete;
    void initialize();
    void poll();
    void ready(uint32_t run, const Ads127Config& adsConfigRequested) override;
    void startFailed(uint32_t run, StartFailure reason, int error) override;
    void destinationFull(uint32_t run) override;
    void stopped(const RecordingResult& recordingResult) override;
    void decoderStalled(uint32_t run) override;
private:
    void handleLine(const char* line);
    void reply(const char* format, ...);
    RecordingApplication& _recordingApplication;
    RecordingReporter& _recordingReporter;
    RecordingConsoleCommands* _recordingConsoleCommands;
    BenchLine _benchLine;
    static constexpr uart_port_t _featherPort = UART_NUM_1;
    QueueHandle_t _uartEvents = nullptr;
};
