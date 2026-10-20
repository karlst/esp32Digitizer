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
 * @file CaptureProfile.h
 * @brief Optional Core 0-owned capture diagnostics; no new worker or ISR work.
 * Read begin(), poll(), finish(), report() in that order. All calls come from
 * the main control task. Conditional compilation removes this class entirely
 * when CONFIG_P4_CAPTURE_PROFILE is off. It never owns capture or disk buffers.
 */
#pragma once
#include "sdkconfig.h"
#if CONFIG_P4_CAPTURE_PROFILE
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "profileMath.h"
#include "IntervalTotals.h"

/**
 * @brief Sample five persistent tasks and estimate each core's spare time.
 * FreeRTOS idle tasks run when no other task is runnable. Busy is elapsed minus
 * idle, not a sum of our three application tasks: drivers/system tasks also run.
 * Runtime accounting includes interrupt time charged to the interrupted task,
 * so these are scheduling estimates, not precise ISR-separated CPU occupancy.
 * The snapshot calls themselves and enabled runtime bookkeeping add overhead.
 */
class CaptureProfile
{
public:
    void begin(TaskHandle_t control, TaskHandle_t decoder, TaskHandle_t writer);
    void poll();
    void finish();
    void report();
private:
    void sample(bool baseline);
    static constexpr unsigned _taskCount = 5;
    TaskHandle_t _handles[_taskCount] = {};
    uint64_t _previousRuntime[_taskCount] = {};
    profileMath::IntervalTotals _intervalTotalsTasks[_taskCount] = {};
    profileMath::IntervalTotals _intervalTotalsBusy[2] = {};
    int64_t _previousAt = 0;
    uint64_t _minWindowUs = 0, _maxWindowUs = 0, _maxSnapshotUs = 0;
    size_t _minInternalFree = 0, _minPsramFree = 0;
    uint32_t _intervals = 0, _invalidIntervals = 0, _boundaryOvershoots = 0;
    bool _running = false;
};
#endif
