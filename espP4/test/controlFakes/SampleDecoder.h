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
 * @file SampleDecoder.h
 * Host replacement for the Core 1 task, with no thread scheduling.
 */
#pragma once
#include "FrameDecoder.h"
#include "CaptureDestination.h"
#include "freertos/task.h"
#include "esp_timer.h"

/**
 * Record configuration/draining and supply results to the real application.
 */
class SampleDecoder
{
public:
    FrameDecoder _frameDecoder;
    bool _drainOk = true;
    uint32_t _clocksPerFrame = 0;

    /* **
     * initialize - No real task is created.
     */
    void initialize()
    {
    }

    /* **
     * configure - Save the requested frame length; tests separately arrange decoder
     * results.
     */
    void configure(CaptureDestination&, uint32_t clocksPerFrame)
    {
        _clocksPerFrame = clocksPerFrame;
    }

    /* **
     * drain - Record draining and return the injected completion outcome.
     */
    bool drain()
    {
        controlFake::operations += "drain ";
        return _drainOk;
    }

    /* **
     * getFrameDecoder - Return the injected frozen decoder state.
     */
    const FrameDecoder& getFrameDecoder() const
    {
        return _frameDecoder;
    }

    /* **
     * getTaskHandle - No host task exists.
     */
    TaskHandle_t getTaskHandle() const
    {
        return nullptr;
    }
};
