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
 * @file RecordingState.h
 * Small control-state snapshot safe to request on the Core 0 control task.
 */
#pragma once
#include <cstdint>
#include "Ads127Config.h"
#include "esp_err.h"

/**
 * Describe receiver state, not completion of asynchronous disk operations.
 * Active means receiving was requested, not proof that data arrived. Run/configuration
 * identify the last accepted recording even after it stops. StartError contains
 * the most recent hardware setup result. Access is restricted to Core 0 control.
 */
struct RecordingState
{
    bool _initialized = false;
    bool _preparing = false;
    bool _active = false;
    // SD/RAM selection stays fixed until reset, including after preparation
    // failure. This is a configuration choice, not a thread synchronization lock.
    bool _recordingDestinationLocked = false;
    uint32_t _run = 0;
    Ads127Config _adsConfig = adsConfig;
    esp_err_t _startError = ESP_OK;
};
