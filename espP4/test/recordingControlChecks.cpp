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

/** @file recordingControlChecks.cpp
 * @brief Check real recording lifecycle with fake hardware, storage and time.
 * This validates sequencing and failure policy, not interrupts, cores or card I/O.
 */
#include "RecordingApplication.h"
#include "AdcReceiver.h"
#include "SampleDecoder.h"
#include "ControlDestination.h"
#include "ControlEvents.h"
#include "RecordingTiming.h"
#include <cassert>
#include <cstdio>

/** @brief Complete simulated asynchronous preparation, then confirm receiving started. */
void startReady(RecordingApplication& recordingApplication, ControlDestination& controlDestination, uint32_t run)
{
    assert(recordingApplication.start(run, adsConfig) == RecordingApplication::StartResult::preparing);
    controlDestination._ready = true;
    recordingApplication.poll();
    assert(recordingApplication.getRecordingState()._active);
}

/** @brief Exercise controller-independent operations; exit nonzero on any violated expectation. */
int main()
{
    AdcReceiver adcReceiver;
    SampleDecoder sampleDecoder;
    ControlDestination controlDestination;
    ControlDestination controlDestinationOther;
    ControlEvents controlEvents;
    RecordingApplication recordingApplication(adcReceiver, sampleDecoder, controlDestination, 10000);
    recordingApplication.setEvents(controlEvents);
    using StartResult = RecordingApplication::StartResult;
    using Reason = RecordingResult::Reason;

    // Public operations refuse use before initialization, without touching storage.
    assert(recordingApplication.start(1, adsConfig) == StartResult::notInitialized);
    assert(controlDestination._preparations == 0 && adcReceiver._starts == 0);
    recordingApplication.initialize();
    recordingApplication.initialize();
    assert(recordingApplication.getRecordingState()._initialized);

    // One start accepts preparation, but cannot enable hardware before storage
    // completes. The same pending request neither repeats setup nor grants READY.
    assert(recordingApplication.start(0, adsConfig) == StartResult::invalidRequest);
    Ads127Config adsConfigInvalid = adsConfig;
    adsConfigInvalid._filterCode ^= 1;
    assert(recordingApplication.start(1, adsConfigInvalid) == StartResult::invalidRequest);
    assert(controlDestination._preparations == 0 && adcReceiver._starts == 0);
    Ads127Config adsConfigRequested = adsConfig;
    assert(recordingApplication.start(1, adsConfigRequested) == StartResult::preparing);
    adsConfigRequested._samplesPerSecond = 0; // Caller may reuse its local object.
    assert(recordingApplication.getRecordingState()._adsConfig.matches(adsConfig));
    assert(controlDestination._adsConfig.matches(adsConfig));
    assert(!recordingApplication.getIdle() && recordingApplication.getRecordingState()._preparing);
    assert(recordingApplication.isRecordingDestinationLocked());
    assert(!recordingApplication.selectDestination(controlDestinationOther, 100));
    assert(recordingApplication.start(1, adsConfig) == StartResult::alreadyPreparing);
    assert(recordingApplication.start(2, adsConfig) == StartResult::busy);
    recordingApplication.poll();
    assert(controlDestination._preparations == 1 && adcReceiver._starts == 0 && controlEvents._readyCount == 0);

    // Preparation lasts much longer than this test's ten-second recording
    // deadline. The recording deadline must not begin until the receiver starts.
    controlFake::nowUs += 60000000;
    recordingApplication.poll();
    assert(recordingApplication.getRecordingState()._preparing && adcReceiver._starts == 0);
    controlDestination._ready = true;
    recordingApplication.poll();
    assert(recordingApplication.getRecordingState()._active && controlEvents._readyCount == 1);
    assert(adcReceiver._clockHz == 32768000 && sampleDecoder._clocksPerFrame == 32);
    assert(controlEvents._adsConfig.matches(adsConfig));
    assert(recordingApplication.start(1, *getAds127Config(16000)) == StartResult::busy);
    assert(recordingApplication.start(1, adsConfig) == StartResult::alreadyRunning);
    assert(recordingApplication.start(2, adsConfig) == StartResult::busy);
    assert(adcReceiver._starts == 1);

    // A normal stop must shut hardware down before draining, notify the
    // controller before finishing storage, and be harmless when repeated.
    sampleDecoder._frameDecoder._captureStats._words = 10;
    controlFake::operations.clear();
    recordingApplication.stop();
    assert(controlFake::operations == "stop drain event finish ");
    assert(controlDestination._clean && controlDestination._samples == 10);
    recordingApplication.stop();
    assert(controlDestination._finishes == 1);

    // A lower-rate request must carry its whole configuration through the
    // application rather than accidentally reusing the default clock spacing.
    const Ads127Config& adsConfigSlow = *getAds127Config(16000);
    assert(recordingApplication.start(100, adsConfigSlow) == StartResult::preparing);
    controlDestination._ready = true;
    recordingApplication.poll();
    assert(adcReceiver._clockHz == 4096000 && sampleDecoder._clocksPerFrame == 256);
    assert(controlDestination._adsConfig.matches(adsConfigSlow));
    assert(controlEvents._adsConfig.matches(adsConfigSlow));
    recordingApplication.stop();
    assert(controlEvents._recordingResult._sampleRate == 16000);

    // Full RAM requests source shutdown only once; a missing acknowledgement
    // becomes a fault after two seconds rather than silently succeeding.
    startReady(recordingApplication, controlDestination, 2);
    controlDestination._full = true;
    recordingApplication.poll();
    recordingApplication.poll();
    assert(controlEvents._fullRequests == 1 && !recordingApplication.getIdle());
    controlFake::nowUs += 2000001;
    recordingApplication.poll();
    assert(recordingApplication.getIdle() && !controlDestination._clean);
    assert(controlEvents._recordingResult._reason == Reason::fault);
    controlDestination._full = false;

    // Repeated start halfway through a run must not postpone its deadline.
    startReady(recordingApplication, controlDestination, 3);
    controlFake::nowUs += 5000000;
    assert(recordingApplication.start(3, adsConfig) == StartResult::alreadyRunning);
    controlFake::nowUs += 5000000;
    recordingApplication.poll();
    assert(controlEvents._recordingResult._reason == Reason::timeout && recordingApplication.getIdle());

    // Loss of command integrity is a recording failure, not a clean STOP.
    startReady(recordingApplication, controlDestination, 4);
    recordingApplication.commandFailed();
    assert(controlEvents._recordingResult._commandErrors == 1 && !controlDestination._clean);

    // Receive or storage failures must stop an otherwise active run. A normal
    // STOP also cannot mark rejected output, bad headers or empty input clean.
    startReady(recordingApplication, controlDestination, 7);
    adcReceiver._failed = true;
    recordingApplication.poll();
    assert(recordingApplication.getIdle() && controlEvents._recordingResult._reason == Reason::fault);
    adcReceiver._failed = false;
    startReady(recordingApplication, controlDestination, 8);
    controlDestination._failed = true;
    recordingApplication.poll();
    assert(recordingApplication.getIdle() && !controlDestination._clean);
    controlDestination._failed = false;
    for (unsigned condition = 0; condition < 3; ++condition)
    {
        startReady(recordingApplication, controlDestination, 9 + condition);
        sampleDecoder._frameDecoder._outputFailed = condition == 0;
        sampleDecoder._frameDecoder._captureStats._badHeaders = condition == 1 ? 1 : 0;
        sampleDecoder._frameDecoder._captureStats._words = condition == 2 ? 0 : 10;
        recordingApplication.stop();
        assert(!controlDestination._clean);
    }
    sampleDecoder._frameDecoder._captureStats._words = 10;

    // Rejected preparation cannot touch hardware. Selection stays locked even
    // after a refusal, because this flag chooses SD/RAM for the whole boot.
    controlDestination._acceptPreparation = false;
    assert(recordingApplication.start(20, adsConfig) == StartResult::storageBusy);
    assert(recordingApplication.getIdle() && recordingApplication.isRecordingDestinationLocked());
    controlDestination._acceptPreparation = true;

    // A disk error, timeout, user stop or broken command link while preparing
    // cancels the pending start. Even an injected late READY cannot enable DMA.
    for (unsigned condition = 0; condition < 4; ++condition)
    {
        const unsigned startsBefore = adcReceiver._starts;
        const unsigned readyBefore = controlEvents._readyCount;
        assert(recordingApplication.start(30 + condition, adsConfig) == StartResult::preparing);
        if (condition == 0) { controlDestination._failed = true; }
        else if (condition == 1)
        {
            controlFake::nowUs += static_cast<int64_t>(recordingTiming::preparationTimeoutMs) * 1000;
            assert(recordingApplication.start(31, adsConfig) == StartResult::alreadyPreparing);
            controlDestination._ready = true;
        }
        else if (condition == 2) { recordingApplication.stop(); }
        else { recordingApplication.commandFailed(); }
        recordingApplication.poll();
        assert(controlDestination._cancelled && recordingApplication.getIdle());
        const RecordingEvents::StartFailure expected[] = {
            RecordingEvents::StartFailure::preparation, RecordingEvents::StartFailure::preparationTimeout,
            RecordingEvents::StartFailure::cancelled, RecordingEvents::StartFailure::commandLink};
        assert(controlEvents._startFailure == expected[condition]);
        controlDestination._ready = true;
        recordingApplication.poll();
        assert(adcReceiver._starts == startsBefore && controlEvents._readyCount == readyBefore);
    }
    controlDestination._failed = false;

    // Setup failure finishes the prepared destination only after queued work
    // drains. A stalled drain aborts before finishing potentially live data.
    adcReceiver._startResult = ESP_FAIL;
    assert(recordingApplication.start(5, adsConfig) == StartResult::preparing);
    controlDestination._ready = true;
    controlFake::operations.clear();
    recordingApplication.poll();
    assert(controlFake::operations == "drain finish startFailed " && !controlDestination._clean);
    assert(controlEvents._startFailure == RecordingEvents::StartFailure::receiver);
    adcReceiver._startResult = ESP_OK;
    startReady(recordingApplication, controlDestination, 6);
    sampleDecoder._drainOk = false;
    controlFake::operations.clear();
    bool aborted = false;
    try { recordingApplication.stop(); }
    catch (const std::runtime_error&) { aborted = true; }
    assert(aborted && controlEvents._stalls == 1);
    assert(controlFake::operations == "stop drain ");
    std::puts("PASS: automatic preparation, delayed readiness, cancellation, stop order and failures.");
    return 0;
}
