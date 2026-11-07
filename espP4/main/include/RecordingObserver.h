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
 * @file RecordingObserver.h
 * Optional lifecycle and timing observations supplied by startup code.
 */
#pragma once
#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * Observe recording without owning raw buffers. A diagnostic may additionally
 * supply a checked initial FSYNC level before decoding; the default preserves
 * ordinary startup. Observations never modify the decoded voltage values.
 * Core 0 owns all calls except decoded and paused, which run on Core 1.
 * started precedes READY; stopped follows receiver disable and decoder drain.
 * Timing fields must not be read on Core 0 until stopped. No observer is called
 * from the receive interrupt. A null observer removes all optional callback work.
 */
class RecordingObserver
{
public:
    /* **
     * getInitialFsyncLow - Count a first captured HIGH as frame zero.
     * False preserves ordinary mid-stream startup. A test may return true only
     * after arming an independent pulse counter while the stopped ADC holds
     * FSYNC LOW; it must check again before READY permits ADC start.
     */
    virtual bool getInitialFsyncLow() const
    {
        return false;
    }

    /* **
     * getDiscardStartupFrame - Withhold the first frame in a checked stopped
     * startup. The diagnostic must account for that pulse when numbering words.
     * False leaves ordinary recording and its error checks unchanged.
     */
    virtual bool getDiscardStartupFrame() const
    {
        return false;
    }

    /* **
     * ~RecordingObserver - Allow destruction through the interface.
     */
    virtual ~RecordingObserver() = default;

    /* **
     * started - Reset measurements after setup and before READY is sent.
     */
    virtual void started(TaskHandle_t control, TaskHandle_t decoder)
    {
        (void)control;
        (void)decoder;
    }

    /* **
     * decoded - Observe one completed decoder call; all durations are microseconds.
     */
    virtual void decoded(int64_t waitUs, int64_t decodeUs, uint32_t bytes)
    {
        (void)waitUs;
        (void)decodeUs;
        (void)bytes;
    }

    /* **
     * paused - Observe an already completed scheduler pause on Core 1.
     */
    virtual void paused(int64_t elapsedUs)
    {
        (void)elapsedUs;
    }

    /* **
     * poll - Sample optional control-task diagnostics while capture is active.
     */
    virtual void poll()
    {
    }

    /* **
     * stopping - Freeze measurements before receiver shutdown begins.
     */
    virtual void stopping()
    {
    }

    /* **
     * stopped - Report after all queued decoding ends; rate is samples/second.
     */
    virtual void stopped(uint32_t rate, uint32_t slots, int64_t gapUs, int64_t copyUs)
    {
        (void)rate;
        (void)slots;
        (void)gapUs;
        (void)copyUs;
    }
};
