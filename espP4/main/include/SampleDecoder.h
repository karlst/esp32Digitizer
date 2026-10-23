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
 * Core 1 task connecting raw receiver blocks to a sample destination.
 */
#pragma once
#include "AdcReceiver.h"
#include "FrameDecoder.h"
#include "CaptureDestination.h"
#include "RecordingObserver.h"
#include "freertos/semphr.h"

/**
 * Decode blocks in order, submit samples, then release each raw block.
 * A task is a software thread scheduled on a core. This permanent task runs on
 * Core 1; initialize, configure and drain are called on Core 0. Configure and
 * statistics access require the previous run to have drained. Queue/semaphore
 * operations synchronize the two cores; no caller may change the destination
 * while the task is processing blocks. All referenced objects live until reset.
 */
class SampleDecoder
{
public:
    SampleDecoder(AdcReceiver& adcReceiver, RecordingObserver* recordingObserver = nullptr);
    SampleDecoder(const SampleDecoder&) = delete;
    SampleDecoder& operator=(const SampleDecoder&) = delete;
    void initialize();
    void configure(CaptureDestination& captureDestination, uint32_t clocksPerFrame);
    bool drain();

    /* **
     * getFrameDecoder - Read decoder results only after drain succeeds.
     */
    const FrameDecoder& getFrameDecoder() const
    {
        return _frameDecoder;
    }

    /* **
     * getTaskHandle - Return task identity for optional profiling; no ownership is
     * transferred.
     */
    TaskHandle_t getTaskHandle() const
    {
        return _decoderTaskHandle;
    }

private:
    static void decodeTask(void* context);
    static bool recordWords(const uint32_t* words, size_t count, void* context);
    void decode();
    AdcReceiver& _adcReceiver;
    CaptureDestination* _captureDestination = nullptr;
    RecordingObserver* _recordingObserver;
    FrameDecoder _frameDecoder;
    SemaphoreHandle_t _drained = nullptr;
    TaskHandle_t _decoderTaskHandle = nullptr;
};
