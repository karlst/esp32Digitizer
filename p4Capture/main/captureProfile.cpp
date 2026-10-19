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
 * @file captureProfile.cpp
 * @brief Quiet one-second runtime sampling, followed by a post-capture report.
 * vTaskGetInfo(..., pdFALSE, eInvalid) reads task information without scanning
 * the stack. Unlike uxTaskGetSystemState(), this avoids scanning every stack
 * during acquisition. Task handles remain valid for the lifetime of this app.
 * ESP-IDF 5.5.6's runtime counter clock must be esp_timer (microseconds) and
 * counters must be 64-bit. The build refuses other settings instead of silently
 * reporting cycle counts as microseconds or risking a long-idle clock rollover.
 */
#include "captureProfile.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <cstdio>

#if !CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS || !CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
#error "Capture profiling requires FreeRTOS runtime statistics using esp_timer"
#endif
static_assert(sizeof(configRUN_TIME_COUNTER_TYPE) == 8,
              "Profiling requires CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64=y");

/**
 * @brief Reset measurements after successful receiver setup, before READY reply.
 * Arguments identify the persistent control, decoder and sdWriter tasks. The
 * interval includes ARM/START handshake and ends at stop entry, excluding file
 * preparation and post-stop CRC verification. This is not an ADC sample timer.
 */
void captureProfile::begin(TaskHandle_t control, TaskHandle_t decoder, TaskHandle_t writer)
{
    *this = captureProfile{};
    handles[0] = xTaskGetIdleTaskHandleForCPU(0);
    handles[1] = xTaskGetIdleTaskHandleForCPU(1);
    handles[2] = decoder;
    handles[3] = writer;
    handles[4] = control;
    running = true;
    sample(true);
}

/**
 * @brief Sample when at least one second elapsed; never wait or print here.
 * Main calls this during capture. Actual window lengths expose delayed polling;
 * no attempt is made to fabricate missed one-second windows after a stall.
 */
void captureProfile::poll()
{
    if (running && esp_timer_get_time() - previousAt >= 1000000) { sample(false); }
}

/** @brief Take the remaining partial interval before DMA teardown, then freeze totals. */
void captureProfile::finish()
{
    if (running)
    {
        sample(false);
        running = false;
    }
}

/**
 * @brief Read task counters and available memory without allocating or printing.
 * Counter differences use actual microsecond duration. Invalid snapshots are
 * excluded and reported; a missing handle must never look like 100% spare CPU.
 * Small per-task overruns of elapsed time are counted separately: scheduled
 * runtime is committed at task switches, not atomically with our timestamp.
 * Memory minima are sampled during this run, not heap minima since boot.
 */
void captureProfile::sample(bool baseline)
{
    const int64_t began = esp_timer_get_time();
    uint64_t current[taskCount] = {};
    bool valid = true;
    for (unsigned index = 0; index < taskCount; ++index)
    {
        if (handles[index] != nullptr)
        {
            TaskStatus_t info = {};
            vTaskGetInfo(handles[index], &info, pdFALSE, eInvalid);
            current[index] = info.ulRunTimeCounter;
            if (!baseline && current[index] < previousRuntime[index]) { valid = false; }
        }
        else { valid = false; }
    }

    // Include both cores' complete scheduled activity through their idle tasks.
    // Five snapshots are sequential, so boundary precision is not sub-tick.
    const int64_t now = esp_timer_get_time();
    const uint64_t duration = static_cast<uint64_t>(now - previousAt);
    if (!baseline && valid && duration != 0)
    {
        for (unsigned index = 0; index < taskCount; ++index)
        {
            const uint64_t delta = current[index] - previousRuntime[index];
            taskTotals[index].add(duration, delta);
            if (delta > duration) { ++boundaryOvershoots; }
            if (index < 2)
            {
                const uint64_t idle = delta < duration ? delta : duration;
                busyTotals[index].add(duration, duration - idle);
            }
        }
        if (intervals == 0 || duration < minWindowUs) { minWindowUs = duration; }
        if (duration > maxWindowUs) { maxWindowUs = duration; }
        ++intervals;
    }
    else if (!valid || (!baseline && duration == 0)) { ++invalidIntervals; }

    // Rebaseline even after an invalid interval; all diagnostics stay explicit.
    for (unsigned index = 0; index < taskCount; ++index)
    {
        previousRuntime[index] = current[index];
    }
    previousAt = now;
    const size_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (baseline || internalFree < minInternalFree) { minInternalFree = internalFree; }
    if (baseline || psramFree < minPsramFree) { minPsramFree = psramFree; }
    const uint64_t cost = static_cast<uint64_t>(esp_timer_get_time() - began);
    if (cost > maxSnapshotUs) { maxSnapshotUs = cost; }
}

/**
 * @brief Print frozen runtime totals and scan task stacks only after capture drains.
 * CPU/task percentages use 100% PER CORE, not 100% for the combined machine.
 * Stack minimum-free bytes are lifetime high-water readings (since task creation),
 * not this run only. Idle task stacks are included. No claim is made about the
 * separate interrupt stack. printf uses integer tenths to avoid float formatting.
 */
void captureProfile::report()
{
    const char* names[taskCount] = {"idle0", "idle1", "decode", "sdWriter", "control"};
    printf("PROFILE capture intervals=%lu invalid=%lu boundaryOvershoots=%lu measuredUs=%llu\n",
           static_cast<unsigned long>(intervals), static_cast<unsigned long>(invalidIntervals),
           static_cast<unsigned long>(boundaryOvershoots),
           static_cast<unsigned long long>(busyTotals[0].elapsedUs));
    if (intervals == 0)
    {
        printf("PROFILE unavailable: no valid runtime intervals; do not infer CPU headroom.\n");
    }
    for (unsigned core = 0; core < 2 && intervals != 0; ++core)
    {
        const auto& totals = busyTotals[core];
        const uint32_t busy = profileMath::percentTenths(totals.executedUs, totals.elapsedUs);
        const uint32_t idle = 1000 - busy;
        printf("PROFILE core=%u busy=%lu.%lu%% idle=%lu.%lu%% worstBusy=%lu.%lu%% peakWindows=%lu\n",
               core, static_cast<unsigned long>(busy / 10), static_cast<unsigned long>(busy % 10),
               static_cast<unsigned long>(idle / 10), static_cast<unsigned long>(idle % 10),
               static_cast<unsigned long>(totals.maxTenths / 10),
               static_cast<unsigned long>(totals.maxTenths % 10),
               static_cast<unsigned long>(totals.peakWindows));
    }
    for (unsigned index = 0; index < taskCount && intervals != 0; ++index)
    {
        const auto& totals = taskTotals[index];
        const uint32_t used = profileMath::percentTenths(totals.executedUs, totals.elapsedUs);
        const UBaseType_t freeStack = handles[index] != nullptr ?
            uxTaskGetStackHighWaterMark(handles[index]) : 0;
        printf("PROFILE task=%s oneCore=%lu.%lu%% stackMinFreeBytes=%lu\n", names[index],
               static_cast<unsigned long>(used / 10), static_cast<unsigned long>(used % 10),
               static_cast<unsigned long>(freeStack));
    }
    printf("PROFILE windowUs min=%llu max=%llu snapshotMaxUs=%llu internalMinFree=%lu psramMinFree=%lu\n",
           static_cast<unsigned long long>(minWindowUs), static_cast<unsigned long long>(maxWindowUs),
           static_cast<unsigned long long>(maxSnapshotUs), static_cast<unsigned long>(minInternalFree),
           static_cast<unsigned long>(minPsramFree));
    printf("PROFILE estimates include measurement overhead; ISR time is not separated. "
           "Worst uses windows >=0.5s; no valid intervals means unavailable. "
           "Review TIMING, queueHigh, SD ringPeak and errors as well.\n");
}
