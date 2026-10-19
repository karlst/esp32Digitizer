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
 * @file captureProfile.h
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

/**
 * @brief Sample five persistent tasks and estimate each core's spare time.
 * FreeRTOS idle tasks run when no other task is runnable. Busy is elapsed minus
 * idle, not a sum of our three application tasks: drivers/system tasks also run.
 * Runtime accounting includes interrupt time charged to the interrupted task,
 * so these are scheduling estimates, not precise ISR-separated CPU occupancy.
 * The snapshot calls themselves and enabled runtime bookkeeping add overhead.
 */
class captureProfile
{
public:
    void begin(TaskHandle_t control, TaskHandle_t decoder, TaskHandle_t writer);
    void poll();
    void finish();
    void report();
private:
    void sample(bool baseline);
    static constexpr unsigned taskCount = 5;
    TaskHandle_t handles[taskCount] = {};
    uint64_t previousRuntime[taskCount] = {};
    profileMath::intervalTotals taskTotals[taskCount] = {};
    profileMath::intervalTotals busyTotals[2] = {};
    int64_t previousAt = 0;
    uint64_t minWindowUs = 0, maxWindowUs = 0, maxSnapshotUs = 0;
    size_t minInternalFree = 0, minPsramFree = 0;
    uint32_t intervals = 0, invalidIntervals = 0, boundaryOvershoots = 0;
    bool running = false;
};
#endif
