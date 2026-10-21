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

/** @file RecordingObserver.h
 * @brief Optional lifecycle and timing observations supplied by startup code.
 */
#pragma once
#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * @brief Observe recording without owning raw buffers or changing decoded samples.
 * Core 0 owns all calls except decoded and paused, which run on Core 1.
 * started precedes READY; stopped follows receiver disable and decoder drain.
 * Timing fields must not be read on Core 0 until stopped. No observer is called
 * from the receive interrupt. A null observer removes all optional callback work.
 */
class RecordingObserver
{
public:

    /** @brief Allow destruction through the interface. */
    virtual ~RecordingObserver() = default;

    /** @brief Reset measurements after setup and before READY is sent. */
    virtual void started(TaskHandle_t control, TaskHandle_t decoder)
    { (void)control; (void)decoder; }

    /** @brief Observe one completed decoder call; all durations are microseconds. */
    virtual void decoded(int64_t waitUs, int64_t decodeUs, uint32_t bytes)
    { (void)waitUs; (void)decodeUs; (void)bytes; }

    /** @brief Observe an already completed scheduler pause on Core 1. */
    virtual void paused(int64_t elapsedUs) { (void)elapsedUs; }

    /** @brief Sample optional control-task diagnostics while capture is active. */
    virtual void poll() {}

    /** @brief Freeze measurements before receiver shutdown begins. */
    virtual void stopping() {}

    /** @brief Report after all queued decoding ends; rate is samples/second. */
    virtual void stopped(uint32_t rate, uint32_t slots, int64_t gapUs, int64_t copyUs)
    { (void)rate; (void)slots; (void)gapUs; (void)copyUs; }
};
