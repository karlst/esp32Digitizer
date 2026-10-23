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
 * @file CaptureProfile.cpp
 * Estimate how busy each P4 core is during capture; print results after stopping.
 * The operating system counts time spent running each task (software thread).
 * Each core has an idle task that runs when no other task needs it. Busy time
 * is elapsed time minus that idle time, not a sum of selected application tasks.
 * begin() takes starting counts, poll() takes roughly one-second readings,
 * finish() freezes them, and report() prints percentages. Core 0 calls all four.
 * Measuring costs CPU time too, and interrupt time is not reported separately.
 * vTaskGetInfo(..., pdFALSE, eInvalid) reads task information without scanning
 * the stack. Unlike uxTaskGetSystemState(), this avoids scanning every stack
 * during acquisition. Task handles remain valid for the lifetime of this app.
 * ESP-IDF 5.5.6's runtime counter clock must be esp_timer (microseconds) and
 * counters must be 64-bit. The build refuses other settings instead of silently
 * reporting cycle counts as microseconds or risking a long-idle clock rollover.
 */
#include "CaptureProfile.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <cstdio>

#if !CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS || !CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
#error "Capture profiling requires FreeRTOS runtime statistics using esp_timer"
#endif
static_assert(sizeof(configRUN_TIME_COUNTER_TYPE) == 8,
    "Profiling requires CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64=y");

/* **
 * begin - Reset measurements after successful receiver setup, before READY
 * reply. Arguments identify the persistent control, decoder and sdWriter tasks.
 * The interval includes ARM/START handshake and ends at stop entry, excluding
 * file preparation and post-stop CRC verification. This is not an ADC sample
 * timer.
 */
void CaptureProfile::begin(TaskHandle_t control, TaskHandle_t decoder, TaskHandle_t writer)
{
    *this = CaptureProfile{};
    _handles[0] = xTaskGetIdleTaskHandleForCore(0);
    _handles[1] = xTaskGetIdleTaskHandleForCore(1);
    _handles[2] = decoder;
    _handles[3] = writer;
    _handles[4] = control;
    _running = true;
    sample(true);
}

/* **
 * poll - Sample when at least one second elapsed; never wait or print here. Main
 * calls this during capture. Actual window lengths expose delayed polling; no
 * attempt is made to fabricate missed one-second windows after a stall.
 */
void CaptureProfile::poll()
{
    if (_running && esp_timer_get_time() - _previousAt >= 1000000)
    {
        sample(false);
    }
}

/* **
 * finish - Take the remaining partial interval before DMA teardown, then freeze
 * totals.
 */
void CaptureProfile::finish()
{
    if (_running)
    {
        sample(false);
        _running = false;
    }
}

/* **
 * sample - Read cumulative task timers and available RAM without printing.
 * baseline=true saves starting readings without adding an interval. Otherwise
 * subtract the previous readings to find time used during this interval. A
 * handle identifies a task; it must remain valid while profiling is active.
 * Counter differences use actual microsecond duration. Invalid snapshots are
 * excluded and reported; a missing handle must never look like 100% spare CPU.
 * Small per-task overruns of elapsed time are counted separately: scheduled
 * runtime is committed at task switches, not atomically with our timestamp.
 * Memory minima are sampled during this run, not heap minima since boot.
 */
void CaptureProfile::sample(bool baseline)
{
    const int64_t began = esp_timer_get_time();
    uint64_t current[_taskCount] = {};
    bool valid = true;
    for (unsigned iTask = 0; iTask < _taskCount; ++iTask)
    {
        if (_handles[iTask] != nullptr)
        {
            TaskStatus_t info = {};
            vTaskGetInfo(_handles[iTask], &info, pdFALSE, eInvalid);
            current[iTask] = info.ulRunTimeCounter;
            if (!baseline && current[iTask] < _previousRuntime[iTask])
            {
                valid = false;
            }
        }
        else
        {
            valid = false;
        }
    }

    // Include both cores' complete scheduled activity through their idle tasks.
    // Five snapshots are sequential, so boundary precision is not sub-tick.
    const int64_t now = esp_timer_get_time();
    const uint64_t duration = static_cast<uint64_t>(now - _previousAt);
    if (!baseline && valid && duration != 0)
    {
        for (unsigned iTask = 0; iTask < _taskCount; ++iTask)
        {
            const uint64_t delta = current[iTask] - _previousRuntime[iTask];
            _intervalTotalsTasks[iTask].add(duration, delta);
            if (delta > duration)
            {
                ++_boundaryOvershoots;
            }
            if (iTask < 2)
            {
                const uint64_t idle = delta < duration ? delta : duration;
                _intervalTotalsBusy[iTask].add(duration, duration - idle);
            }
        }
        if (_intervals == 0 || duration < _minWindowUs)
        {
            _minWindowUs = duration;
        }
        if (duration > _maxWindowUs)
        {
            _maxWindowUs = duration;
        }
        ++_intervals;
    }
    else if (!valid || (!baseline && duration == 0))
    {
        ++_invalidIntervals;
    }

    // Use these readings as the next starting point even after an invalid
    // interval, so a bad reading does not contaminate every later interval.
    for (unsigned iTask = 0; iTask < _taskCount; ++iTask)
    {
        _previousRuntime[iTask] = current[iTask];
    }
    _previousAt = now;
    const size_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (baseline || internalFree < _minInternalFree)
    {
        _minInternalFree = internalFree;
    }
    if (baseline || psramFree < _minPsramFree)
    {
        _minPsramFree = psramFree;
    }
    const uint64_t cost = static_cast<uint64_t>(esp_timer_get_time() - began);
    if (cost > _maxSnapshotUs)
    {
        _maxSnapshotUs = cost;
    }
}

/* **
 * report - Print frozen runtime totals and scan task stacks only after capture
 * drains. CPU/task percentages use 100% PER CORE, not 100% for the combined
 * machine. Stack minimum-free bytes are lifetime high-water readings (since task
 * creation), not this run only. Idle task stacks are included. No claim is made
 * about the separate interrupt stack. printf uses integer tenths to avoid float
 * formatting.
 */
void CaptureProfile::report()
{
    const char* names[_taskCount] = {"idle0", "idle1", "decode", "sdWriter", "control"};
    printf("PROFILE capture intervals=%lu invalid=%lu boundaryOvershoots=%lu measuredUs=%llu\n",
        static_cast<unsigned long>(_intervals), static_cast<unsigned long>(_invalidIntervals),
        static_cast<unsigned long>(_boundaryOvershoots),
        static_cast<unsigned long long>(_intervalTotalsBusy[0]._elapsedUs));
    if (_intervals == 0)
    {
        printf("PROFILE unavailable: no valid runtime intervals; do not infer CPU headroom.\n");
    }
    for (unsigned iCore = 0; iCore < 2 && _intervals != 0; ++iCore)
    {
        const auto& intervalTotals = _intervalTotalsBusy[iCore];
        const uint32_t busy = profileMath::percentTenths(intervalTotals._executedUs, intervalTotals._elapsedUs);
        const uint32_t idle = 1000 - busy;
        printf("PROFILE core=%u busy=%lu.%lu%% idle=%lu.%lu%% worstBusy=%lu.%lu%% peakWindows=%lu\n",
            iCore, static_cast<unsigned long>(busy / 10), static_cast<unsigned long>(busy % 10),
            static_cast<unsigned long>(idle / 10), static_cast<unsigned long>(idle % 10),
            static_cast<unsigned long>(intervalTotals._maxTenths / 10),
            static_cast<unsigned long>(intervalTotals._maxTenths % 10),
            static_cast<unsigned long>(intervalTotals._peakWindows));
    }
    for (unsigned iTask = 0; iTask < _taskCount && _intervals != 0; ++iTask)
    {
        const auto& intervalTotals = _intervalTotalsTasks[iTask];
        const uint32_t used = profileMath::percentTenths(intervalTotals._executedUs, intervalTotals._elapsedUs);
        const UBaseType_t freeStack = _handles[iTask] != nullptr ? uxTaskGetStackHighWaterMark(_handles[iTask]) : 0;
        printf("PROFILE task=%s oneCore=%lu.%lu%% stackMinFreeBytes=%lu\n", names[iTask],
            static_cast<unsigned long>(used / 10), static_cast<unsigned long>(used % 10),
            static_cast<unsigned long>(freeStack));
    }
    printf("PROFILE windowUs min=%llu max=%llu snapshotMaxUs=%llu internalMinFree=%lu psramMinFree=%lu\n",
        static_cast<unsigned long long>(_minWindowUs), static_cast<unsigned long long>(_maxWindowUs),
        static_cast<unsigned long long>(_maxSnapshotUs), static_cast<unsigned long>(_minInternalFree),
        static_cast<unsigned long>(_minPsramFree));
    printf("PROFILE estimates include measurement overhead; ISR time is not separated. "
           "Worst uses windows >=0.5s; no valid intervals means unavailable. "
           "Review TIMING, queueHigh, SD ringPeak and errors as well.\n");
}
