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

/** @file RecordingApplication.h
 * @brief Recording operations and sequencing, with no command or display syntax.
 */
#pragma once
#include "CaptureDestination.h"
#include "RecordingObserver.h"
#include "RecordingEvents.h"
#include "RecordingState.h"
class AdcReceiver;
class SampleDecoder;

/**
 * @brief Coordinate one recording through initialization, preparation, start and stop.
 * A controller calls operations and poll on Core 0. The receiver owns interrupt
 * work, SampleDecoder owns Core 1 decoding, and the destination owns storage.
 * Startup supplies permanent services. This class never reads a console/UART
 * or formats terminal text. A future controller can call the same operations.
 */
class RecordingApplication
{
public:
    enum class StartResult { preparing, alreadyPreparing, alreadyRunning, busy, notInitialized, invalidRequest, storageBusy };
    RecordingApplication(AdcReceiver& adcReceiver, SampleDecoder& sampleDecoder,
        CaptureDestination& captureDestination, uint32_t timeoutMs,
        RecordingObserver* recordingObserver = nullptr);
    RecordingApplication(const RecordingApplication&) = delete;
    RecordingApplication& operator=(const RecordingApplication&) = delete;
    void initialize();
    StartResult start(uint32_t run, const Ads127Config& adsConfigRequested = ::adsConfig);
    void stop(RecordingResult::Reason reason = RecordingResult::Reason::requested);
    void poll();
    void commandFailed();
    void setEvents(RecordingEvents& recordingEvents);
    RecordingState getRecordingState() const;
    bool selectDestination(CaptureDestination& captureDestination, uint32_t timeoutMs);
    bool lockRecordingDestination();
    bool getIdle() const;
    bool isRecordingDestinationLocked() const;
private:
    void drain();
    void startPreparedRecording();
    void failPreparation(RecordingEvents::StartFailure reason);
    int64_t _preparationStartedAt = 0;
    AdcReceiver& _adcReceiver;
    SampleDecoder& _sampleDecoder;
    CaptureDestination* _captureDestination;
    RecordingObserver* _recordingObserver;
    RecordingEvents* _recordingEvents = nullptr;
    uint32_t _timeoutMs;
    RecordingState _recordingState;
    RecordingResult _recordingResult;
    int64_t _armedAt = 0, _stopRequestedAt = 0;
    uint32_t _commandErrors = 0;
};
