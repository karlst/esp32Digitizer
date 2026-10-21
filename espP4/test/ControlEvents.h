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

/** @file ControlEvents.h
 * @brief Controller notifications captured by host sequencing checks.
 */
#pragma once
#include "RecordingEvents.h"
#include "esp_timer.h"

/** @brief Save outcomes without serial communication or terminal reporting. */
class ControlEvents : public RecordingEvents
{
public:
    unsigned _fullRequests = 0, _stalls = 0;
    RecordingResult _recordingResult;
    unsigned _readyCount = 0, _startFailures = 0;
    StartFailure _startFailure = StartFailure::preparation;
    Ads127Config _adsConfig = adsConfig;

    /** @brief Count permission to start the source, separate from preparation acceptance. */
    void ready(uint32_t, const Ads127Config& adsConfigRequested) override
    { ++_readyCount; _adsConfig = adsConfigRequested; }

    /** @brief Retain the reason a pending request could not start. */
    void startFailed(uint32_t, StartFailure reason, int) override
    { ++_startFailures; _startFailure = reason; controlFake::operations += "startFailed "; }

    /** @brief Record a request to stop the source; do not re-enter the application. */
    void destinationFull(uint32_t) override { ++_fullRequests; }

    /** @brief Copy the result to verify it survives later application actions. */
    void stopped(const RecordingResult& recordingResult) override
    { controlFake::operations += "event "; _recordingResult = recordingResult; }

    /** @brief Record the fatal drain failure notification. */
    void decoderStalled(uint32_t) override { ++_stalls; }
};
