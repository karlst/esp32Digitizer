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
 * @file RecordingMonitor.h
 * @brief Keep the last fully validated recording status, owned by Feather's loop.
 */
#pragma once
#include <Arduino.h>
#include "../../../shared/RecordingStatus.h"

/**
 * @brief Format receiver recording telemetry without claiming stale data is current.
 * receiverMonitor commits this object only after validating the entire UART frame.
 * No other task touches it, so it requires no separate mutex.
 */
class RecordingMonitor
{
public:
    // Last fully validated receiver recording values; retained after Stop so the browser can
    // show the completed run. Retaining a copy is not evidence of a live connection.
    RecordingStatus _recordingStatus;
    // True only when the accepted UART frame supports recording statistics.
    // This is protocol capability, not card-present or recording-active status.
    bool _available = false;
    String stateJson(bool connected) const;
};
