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
 * @file RecordingApplication.cpp
 * Coordinate recording without interpreting commands or printing reports.
 * Reading path: initialize -> start -> poll -> startPreparedRecording -> stop.
 * All these calls run on the Core 0 control task. Preparation and finishing
 * request work from the destination; SD work continues on its separate Core 0
 * task. AdcReceiver handles interrupts; SampleDecoder runs on Core 1. A sample
 * is one ADC measurement, received as a status byte plus a 24-bit value.
 * No capture starts at power-up. The controller chooses when to request it.
 */
#include "RecordingApplication.h"
#include "AdcReceiver.h"
#include "SampleDecoder.h"
#include "Ads127Config.h"
#include "RecordingTiming.h"
#include "esp_timer.h"

/* **
 * RecordingApplication - Save references to the receiver, decoder and selected
 * SD or RAM destination. The caller must keep them alive. timeoutMs limits
 * reception time after storage is ready; the optional observer collects test
 * measurements.
 */
RecordingApplication::RecordingApplication(AdcReceiver& adcReceiver, SampleDecoder& sampleDecoder,
    CaptureDestination& captureDestination, uint32_t timeoutMs, RecordingObserver* recordingObserver)
    : _adcReceiver(adcReceiver), _sampleDecoder(sampleDecoder),
      _captureDestination(&captureDestination), _recordingObserver(recordingObserver), _timeoutMs(timeoutMs)
{
}

/* **
 * initialize - Allocate resources once and start the waiting decoder task;
 * repeated calls do nothing.
 */
void RecordingApplication::initialize()
{
    if (!_recordingState._initialized)
    {
        _adcReceiver.initialize();
        _sampleDecoder.initialize();
        _recordingState._initialized = true;
    }
}

/* **
 * setEvents - Choose who receives ready, stopped and failure notifications. Set
 * this before recording starts and keep the recipient alive while this
 * application uses it.
 */
void RecordingApplication::setEvents(RecordingEvents& recordingEvents)
{
    _recordingEvents = &recordingEvents;
}

/* **
 * getRecordingState - Return control state on Core 0; it does not establish that
 * disk work is finished.
 */
RecordingState RecordingApplication::getRecordingState() const
{
    return _recordingState;
}

/* **
 * selectDestination - Choose SD or RAM storage while idle and before the
 * destination has been locked. Save its reception timeout in milliseconds.
 * Return true if selected; this also prevents further changes until reset.
 */
bool RecordingApplication::selectDestination(CaptureDestination& captureDestination, uint32_t timeoutMs)
{
    const bool retVal = !_recordingState._recordingDestinationLocked && getIdle();
    if (retVal)
    {
        _captureDestination = &captureDestination;
        _timeoutMs = timeoutMs;
        _recordingState._recordingDestinationLocked = true;
    }
    return retVal;
}

/* **
 * lockRecordingDestination - Prevent changing between SD and RAM until reset.
 * This is a configuration flag, not a lock between tasks. Return whether
 * preparation and capture are both idle; the flag is set even when this returns
 * false.
 */
bool RecordingApplication::lockRecordingDestination()
{
    _recordingState._recordingDestinationLocked = true;
    return getIdle();
}

/* **
 * getIdle - Return true when neither preparation nor reception is pending; SD
 * may still be finishing.
 */
bool RecordingApplication::getIdle() const
{
    return !_recordingState._active && !_recordingState._preparing;
}

/* **
 * isRecordingDestinationLocked - Report whether destination selection is fixed
 * until reset.
 */
bool RecordingApplication::isRecordingDestinationLocked() const
{
    return _recordingState._recordingDestinationLocked;
}

/* **
 * start - Request a recording with a nonzero run number and a supported ADC
 * configuration. Called by the controller on Core 0. Ask the destination to
 * prepare, save the request, and return its status.
 *
 * This call does not wait for disk preparation or start the receiver. poll
 * checks readiness and starts reception later. Repeating the same request while
 * it is pending or active reports that state without restarting it or extending
 * its deadline.
 */
RecordingApplication::StartResult RecordingApplication::start(uint32_t run, const Ads127Config& adsConfigRequested)
{
    StartResult retVal = StartResult::invalidRequest;

    if (!_recordingState._initialized)
    {
        retVal = StartResult::notInitialized;
    }

    else if (!getIdle())
    {
        const bool same = run == _recordingState._run && adsConfigRequested.matches(_recordingState._adsConfig);

        retVal = !same ? StartResult::busy : _recordingState._active ? StartResult::alreadyRunning
                                                                     : StartResult::alreadyPreparing;
    }

    else if (run != 0 && isSupportedAds127Config(adsConfigRequested))
    {
        // Keep the SD/RAM selection fixed until reset, including if preparation
        // fails. This is a selection flag, not a thread synchronization lock.
        _recordingState._recordingDestinationLocked = true;
        retVal = StartResult::storageBusy;

        if (_captureDestination->prepare(run, adsConfigRequested))
        {
            _recordingState._run = run;
            _recordingState._adsConfig = adsConfigRequested;
            _recordingState._startError = ESP_OK;
            _recordingState._preparing = true;
            _preparationStartedAt = esp_timer_get_time();
            _stopRequestedAt = 0;
            _commandErrors = 0;
            retVal = StartResult::preparing;
        }
    }
    return retVal;
}

/* **
 * startPreparedRecording - Start reception after poll confirms storage is ready.
 * Configure the decoder with the number of ADC clocks per frame, then start the
 * receiver. On success, notify the controller so it can tell Feather to start
 * the ADC.
 *
 * The reception timeout begins here, excluding disk preparation. A ready
 * notification means the P4 is waiting for data; it does not prove that data has
 * arrived. If receiver setup fails, finish any queued decoding and ask storage
 * to close.
 */
void RecordingApplication::startPreparedRecording()
{
    const Ads127Config& adsConfigRequested = _recordingState._adsConfig;
    _sampleDecoder.configure(*_captureDestination, adsConfigRequested._slots * 32U);
    _recordingState._startError = _adcReceiver.start(
        adsConfigRequested._samplesPerSecond * adsConfigRequested._slots * 32U);
    _recordingState._preparing = false;

    if (_recordingState._startError == ESP_OK)
    {
        if (_recordingObserver != nullptr)
        {
            _recordingObserver->started(xTaskGetCurrentTaskHandle(), _sampleDecoder.getTaskHandle());
        }

        _armedAt = esp_timer_get_time();
        _recordingState._active = true;

        if (_recordingEvents != nullptr)
        {
            _recordingEvents->ready(_recordingState._run, adsConfigRequested);
        }
    }
    else
    {
        // Receiver setup already disabled partial hardware setup. Drain any
        // blocks delivered before failure before asking storage to finish.
        drain();
        _captureDestination->finish(_recordingState._run, adsConfigRequested._samplesPerSecond, false, 0);

        if (_recordingEvents != nullptr)
        {
            _recordingEvents->startFailed(_recordingState._run, RecordingEvents::StartFailure::receiver,
                _recordingState._startError);
        }
    }
}

/* **
 * failPreparation - Cancel a pending start; late disk completion must never
 * enable the receiver.
 */
void RecordingApplication::failPreparation(RecordingEvents::StartFailure reason)
{
    _captureDestination->cancelPreparation();
    _recordingState._preparing = false;

    if (_recordingEvents != nullptr)
    {
        _recordingEvents->startFailed(_recordingState._run, reason, ESP_OK);
    }
}

/* **
 * setTimeout - Set the reception timeout in milliseconds while idle and before
 * destination selection is locked. Return false for zero or for a request made
 * too late.
 */
bool RecordingApplication::setTimeout(uint32_t timeoutMs)
{
    const bool retVal = getIdle() && !isRecordingDestinationLocked() && timeoutMs > 0;
    if (retVal)
    {
        _timeoutMs = timeoutMs;
    }
    return retVal;
}

/* **
 * drain - Wait for Core 1 to finish decoding the queued receiver blocks after
 * reception stops. If it does not finish within its deadline, report the failure
 * and abort. Reusing those blocks while decoding continues would corrupt data.
 */
void RecordingApplication::drain()
{
    if (!_sampleDecoder.drain())
    {
        if (_recordingEvents != nullptr)
        {
            _recordingEvents->decoderStalled(_recordingState._run);
        }
        ESP_ERROR_CHECK(ESP_ERR_TIMEOUT);
    }
}

/* **
 * stop - Stop the receiver, wait for queued decoding, and report the acquisition
 * result. The controller is responsible for stopping the ADC itself. If
 * preparation is still pending, cancel it instead.
 *
 * After decoding stops, ask the destination to finish saving. This call does not
 * wait for SD draining, verification or closing. A partly filled hardware
 * receive block may be missing from the final decoded data.
 */
void RecordingApplication::stop(RecordingResult::Reason reason)
{
    if (_recordingState._preparing)
    {
        failPreparation(RecordingEvents::StartFailure::cancelled);
    }

    else if (_recordingState._active)
    {
        if (_recordingObserver != nullptr)
        {
            _recordingObserver->stopping();
        }
        _adcReceiver.stop();
        drain();
        _recordingState._active = false;

        // Both writers of these counters are stopped. Keep the large snapshot
        // in object storage rather than on the control task's small stack.
        const Ads127Config& adsConfigRequested = _recordingState._adsConfig;
        const FrameDecoder& frameDecoder = _sampleDecoder.getFrameDecoder();
        _recordingResult._reason = reason;
        _recordingResult._run = _recordingState._run;
        _recordingResult._sampleRate = adsConfigRequested._samplesPerSecond;
        _recordingResult._slots = adsConfigRequested._slots;
        _recordingResult._captureStats = frameDecoder.getCaptureStats();
        _recordingResult._receiverStats = _adcReceiver.getReceiverStats();
        _recordingResult._pendingBits = frameDecoder.getPendingBits();
        _recordingResult._commandErrors = _commandErrors;
        const auto& receiverStats = _recordingResult._receiverStats;
        const auto& captureStats = _recordingResult._captureStats;

        // Estimate rate between callbacks: four ADC clock positions per raw
        // byte. Exclude the first chunk because its start time was not observed.
        const int64_t interval = receiverStats._lastBlockAt - receiverStats._firstBlockAt;
        _recordingResult._observedRate = interval > 0 ? (receiverStats._rawBytes - receiverStats._firstBlockBytes) * 4.0e6 /
                                                            (interval * adsConfigRequested._slots * 32.0)
                                                      : 0.0;
        _recordingResult._clean = reason == RecordingResult::Reason::requested &&
                                  captureStats._words != 0 && captureStats._badHeaders == 0 &&
                                  captureStats._badPeriods == 0 && captureStats._shortWords == 0 &&
                                  receiverStats._droppedBlocks == 0 && receiverStats._callbackErrors == 0 &&
                                  _commandErrors == 0 && !frameDecoder.getOutputFailed();

        // Required source-control notifications precede optional diagnostics and
        // destination completion, which can print a RAM summary or queue SD work.
        if (_recordingEvents != nullptr)
        {
            _recordingEvents->stopped(_recordingResult);
        }

        if (_recordingObserver != nullptr)
        {
            _recordingObserver->stopped(adsConfigRequested._samplesPerSecond, adsConfigRequested._slots,
                receiverStats._maxCallbackGapUs, receiverStats._maxCopyUs);
        }
        _captureDestination->finish(_recordingState._run, adsConfigRequested._samplesPerSecond,
            _recordingResult._clean, captureStats._words);
    }
}

/* **
 * commandFailed - A controller reports lost/corrupt commands; cancel preparation
 * or stop the active run.
 */
void RecordingApplication::commandFailed()
{
    if (_recordingState._preparing)
    {
        failPreparation(RecordingEvents::StartFailure::commandLink);
    }

    else if (_recordingState._active)
    {
        ++_commandErrors;
        stop(RecordingResult::Reason::fault);
    }
}

/* **
 * poll - Check preparation and recording progress once. Startup calls this
 * repeatedly on Core 0. It does not read commands or move samples.
 *
 * Start the receiver when storage is ready, or cancel if preparation fails or
 * times out. During capture, stop on errors or timeout. When storage fills, ask
 * the controller to stop the ADC and allow two seconds for its response. Also
 * call the optional observer while capture remains active.
 */
void RecordingApplication::poll()
{
    // Preparation has its own deadline. Check failure and deadline before
    // readiness so completion after the allowed interval cannot start capture.
    if (_recordingState._preparing)
    {
        if (_captureDestination->getFailed())
        {
            failPreparation(RecordingEvents::StartFailure::preparation);
        }

        else if (esp_timer_get_time() - _preparationStartedAt >=
                 static_cast<int64_t>(recordingTiming::preparationTimeoutMs) * 1000)
        {
            failPreparation(RecordingEvents::StartFailure::preparationTimeout);
        }

        else if (_captureDestination->getReady())
        {
            startPreparedRecording();
        }
    }

    if (_recordingState._active)
    {
        if (_captureDestination->getFull() && _stopRequestedAt == 0)
        {
            _stopRequestedAt = esp_timer_get_time();
            if (_recordingEvents != nullptr)
            {
                _recordingEvents->destinationFull(_recordingState._run);
            }
        }

        if (_adcReceiver.getFailed() || _captureDestination->getFailed() ||
            (_stopRequestedAt != 0 && esp_timer_get_time() - _stopRequestedAt > 2000000))
        {
            stop(RecordingResult::Reason::fault);
        }

        else if (esp_timer_get_time() - _armedAt >= static_cast<int64_t>(_timeoutMs) * 1000)
        {
            stop(RecordingResult::Reason::timeout);
        }

        if (_recordingState._active && _recordingObserver != nullptr)
        {
            _recordingObserver->poll();
        }
    }
}
