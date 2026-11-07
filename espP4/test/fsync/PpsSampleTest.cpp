/* Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
 * Reading path: initialize reserves GPIO6; start arms rising-edge capture;
 * captured saves hardware times; observe associates numbered FSYNC observations;
 * stop freezes events; report prints only after the real decoder has drained.
 * GPIO6 must receive a safe 3.3-V PPS signal with a shared ground. Never connect
 * a generator output and GPS output to this input at the same time.
 */
#include "PpsSampleTest.h"
#include <cstdio>

/* **
 * initialize - Core 0 allocates capture channel 1 after FSYNC's channel 0.
 * The caller supplies the same hardware timer, so tick counts share an origin.
 * Pull-down biases an unplugged input LOW. Only rising edges are captured;
 * falling edges and pulse width do not participate in this experiment.
 */
void PpsSampleTest::initialize(mcpwm_cap_timer_handle_t timer)
{
    mcpwm_capture_channel_config_t config = {};
    config.gpio_num = 6;
    config.prescale = 1;
    config.flags.pos_edge = true;
    config.flags.pull_down = true;
    ESP_ERROR_CHECK(mcpwm_new_capture_channel(timer, &config, &_channel));
    mcpwm_capture_event_callbacks_t callbacks = {};
    callbacks.on_cap = captured;
    ESP_ERROR_CHECK(mcpwm_capture_channel_register_event_callbacks(_channel, &callbacks, this));
}

/* **
 * start - Core 0 clears the previous run before capture is enabled. The shared
 * timer is already running; this method does not restart it or start the ADC.
 */
void PpsSampleTest::start()
{
    _referencesCount = _nextEvent = _beforeFirst = _late = 0;
    _haveFirst = false;
    _firstTicks = _previousTicks = _wraps = 0;
    _elapsedTicks = 0;
    _boundaryFailures = _badIntervals = 0;
    _missing = false;
    portENTER_CRITICAL(&_mux);
    _count = 0;
    _overflow = false;
    _armed = true;
    portEXIT_CRITICAL(&_mux);
    ESP_ERROR_CHECK(mcpwm_capture_channel_enable(_channel));
}

/* **
 * captured - The driver calls this interrupt service routine (ISR) on Core 0.
 * cap_value was latched by hardware at the edge; interrupt execution time is
 * not the measurement. Copy under a short lock, without printing or allocating.
 * Overflow invalidates the run. False means no task wakeup is requested.
 */
bool PpsSampleTest::captured(mcpwm_cap_channel_handle_t channel,
    const mcpwm_capture_event_data_t* event, void* context)
{
    (void)channel;
    auto& ppsSampleTest = *static_cast<PpsSampleTest*>(context);
    portENTER_CRITICAL_ISR(&ppsSampleTest._mux);
    if (ppsSampleTest._armed)
    {
        if (ppsSampleTest._count < _capacity)
        {
            ppsSampleTest._events[ppsSampleTest._count++] = event->cap_value;
        }
        else
        {
            ppsSampleTest._overflow = true;
        }
    }
    portEXIT_CRITICAL_ISR(&ppsSampleTest._mux);
    return false;
}

/* **
 * stop - Core 0 excludes pending ISR writes before disabling the channel.
 * The caller stops the shared timer afterward. A stopped object can be reported
 * without a lock; no capture callback can modify the saved events anymore.
 */
void PpsSampleTest::stop()
{
    portENTER_CRITICAL(&_mux);
    const bool armed = _armed;
    _armed = false;
    portEXIT_CRITICAL(&_mux);
    if (armed)
    {
        ESP_ERROR_CHECK(mcpwm_capture_channel_disable(_channel));
    }
}

/* **
 * observe - Core 0 supplies a real FSYNC timestamp and its predicted pulse
 * number, never a time inferred from the PPS edge. Consume pending PPS events
 * only when they precede this FSYNC. A future event waits for a later poll.
 * Extend FSYNC time by accumulating differences between millisecond polls,
 * so the complete run can span multiple 32-bit wraps. Compare each pending PPS
 * with the current FSYNC (not the beginning of the run); their expected gap is
 * below 10 ms, far below half the counter range. Discard pulses preceding the
 * first numbered observation explicitly. A delay over 10 ms invalidates mapping.
 * pulse-2 is the stored sample index under the checked startup convention.
 */
void PpsSampleTest::observe(uint32_t pulse, uint32_t ticks)
{
    if (!_haveFirst)
    {
        _firstTicks = _previousTicks = ticks;
        _haveFirst = true;
    }
    else
    {
        _elapsedTicks += uint32_t(ticks - _previousTicks);
        _wraps += ticks < _previousTicks;
        _previousTicks = ticks;
    }
    const uint64_t extendedTicks = uint64_t(_firstTicks) + _elapsedTicks;
    bool waiting = false;
    while (!waiting)
    {
        uint32_t ppsTicks = 0;
        portENTER_CRITICAL(&_mux);
        const bool available = _nextEvent < _count;
        if (available)
        {
            ppsTicks = _events[_nextEvent];
        }
        portEXIT_CRITICAL(&_mux);
        if (!available)
        {
            waiting = true;
        }
        else if (uint32_t(ticks - ppsTicks) > 0x7fffffffU)
        {
            waiting = true;
        }
        else if (uint64_t(uint32_t(ticks - ppsTicks)) > _elapsedTicks)
        {
            // This edge predates the first numbered FSYNC. Use accumulated
            // elapsed time so later wraps cannot resemble startup again.
            ++_beforeFirst;
            ++_nextEvent;
        }
        else
        {
            if (ticks - ppsTicks <= 1600000 && pulse >= 2 && _referencesCount < _capacity)
            {
                auto& reference = _references[_referencesCount];
                reference = {_nextEvent, ppsTicks, ticks, pulse - 2,
                    extendedTicks - uint32_t(ticks - ppsTicks), extendedTicks};

                // Capture an actual adjacent pair after this PPS. The earlier
                // edge ends the previous block; the later one starts the next.
                // Keep the original reference too, so existing comparison rows
                // retain their meaning and the extra numbering is auditable.
                FsyncBoundary boundary;
                reference._boundaryValid = _fsyncBoundaryCapture.capture(pulse, ticks, boundary, reference._readResult);
                if (reference._boundaryValid)
                {
                    reference._tcFsBefore = extendedTicks + uint32_t(boundary._tcBefore - ticks);
                    reference._tcFsAfter = reference._tcFsBefore + uint32_t(boundary._tcAfter - boundary._tcBefore);
                    reference._firstSample = boundary._pulseAfter - 2;
                    reference._boundaryValid = reference._tcFsAfter - reference._ppsTicksExtended <= 1600000;
                }
                if (!reference._boundaryValid)
                {
                    ++_boundaryFailures;
                }
                if (_referencesCount)
                {
                    const uint64_t interval = reference._ppsTicksExtended - _references[_referencesCount - 1]._ppsTicksExtended;
                    _badIntervals += interval < 158400000 || interval > 161600000;
                }
                ++_referencesCount;
            }
            else
            {
                ++_late;
            }
            ++_nextEvent;
        }
    }
    // A missing edge must stop acquisition rather than merely fail the report
    // after a full minute. Allow 1.1 seconds from the first reference or last PPS.
    const uint64_t sincePps = _referencesCount ?
        extendedTicks - _references[_referencesCount - 1]._ppsTicksExtended : _elapsedTicks;
    _missing = _missing || sincePps > 176000000;
}

/* **
 * getFailed - Core 0 checks timing faults during acquisition. Read the ISR's
 * overflow flag under its lock; all remaining fields belong to this same task.
 * No error is cleared until the next stopped-source start.
 */
bool PpsSampleTest::getFailed() const
{
    portENTER_CRITICAL(&_mux);
    const bool overflow = _overflow;
    portEXIT_CRITICAL(&_mux);
    return overflow || _late || _boundaryFailures || _badIntervals || _missing;
}

/* **
 * getBoundary - Core 0 copies a completed observed boundary to the recorder.
 * observe and this call run sequentially on that task; no ISR writes references.
 * False means this index is not available or failed capture, never a guessed pair.
 */
bool PpsSampleTest::getBoundary(unsigned iBoundary, TimedBoundary& boundary) const
{
    const bool retVal = iBoundary < _referencesCount && _references[iBoundary]._boundaryValid;
    if (retVal)
    {
        const auto& reference = _references[iBoundary];
        boundary = {reference._ppsTicksExtended, reference._tcFsBefore,
            reference._tcFsAfter, reference._firstSample};
    }
    return retVal;
}

/* **
 * report - After stop and decoder drain, validate approximately one-second PPS
 * spacing and sample numbering, then print each direct timestamp difference.
 * Require 59..61 mapped pulses, 59..61 captured pulses, and at least two timer
 * wraps during the sixty-second acquisition. One edge can fall outside the
 * numbered window depending on source phase and shutdown timing. Leading
 * and trailing pulses outside numbered observations are reported, not mapped.
 * Reference sample numbers must exist in the decoded stream, although only its
 * first 65536 words were retained by this diagnostic. This class writes no SD
 * file; the optional combined destination writes the complete decoded stream.
 * Adjacent-edge rows must also identify retained samples; their failure prevents
 * a successful report even if the original PPS reference rows look valid.
 * 6.25 ns/tick is nominal; subtract the agreed 2553 ns path correction for the
 * analog-input time relative to PPS. Neither GPS UTC association nor GPS time
 * validity is checked here; an arbitrary 1-Hz input is not proof of GPS lock.
 */
void PpsSampleTest::report(uint32_t run, bool numbered, uint32_t decodedWords) const
{
    unsigned badIntervals = 0;
    unsigned boundaries = 0;
    bool samplesExist = true;
    for (unsigned iEvent = 1; iEvent < _count; ++iEvent)
    {
        const uint32_t interval = _events[iEvent] - _events[iEvent - 1];
        badIntervals += interval < 158400000 || interval > 161600000;
        printf("PPS INTERVAL event=%u ticks=%lu\n", iEvent, (unsigned long)interval);
    }
    for (unsigned iReference = 0; iReference < _referencesCount; ++iReference)
    {
        const auto& reference = _references[iReference];
        samplesExist = samplesExist && reference._sample < decodedWords;
        if (reference._boundaryValid)
        {
            ++boundaries;
            samplesExist = samplesExist && reference._firstSample < decodedWords;
            printf("PPS BOUNDARY event=%lu ppsTicks64=%llu lastSample=%lu lastFsTicks64=%llu firstSample=%lu firstFsTicks64=%llu spacingTicks=%llu\n",
                (unsigned long)reference._event, (unsigned long long)reference._ppsTicksExtended,
                (unsigned long)(reference._firstSample - 1), (unsigned long long)reference._tcFsBefore,
                (unsigned long)reference._firstSample, (unsigned long long)reference._tcFsAfter,
                (unsigned long long)(reference._tcFsAfter - reference._tcFsBefore));
        }
        else
        {
            // Report raw evidence after shutdown, never from the read loop.
            // spacing must be 156 or 157 ticks; anchor age <=1600000 ticks;
            // elapsedCycles must be <7200 (20 us at 360 MHz). numbered also
            // checks the anchor's rounding residual and pulse-number range.
            const auto& readResult = reference._readResult;
            printf("PPS BOUNDARY_REJECT event=%lu referencePulse=%lu referenceTicks=%lu beforeTicks=%lu afterTicks=%lu spacingTicks=%lu anchorAgeTicks=%lu elapsedCycles=%lu numbered=%u ppsAgeTicks=%lu\n",
                (unsigned long)reference._event, (unsigned long)(reference._sample + 2),
                (unsigned long)reference._fsTicks, (unsigned long)readResult._tcBefore,
                (unsigned long)readResult._tcAfter,
                (unsigned long)(readResult._tcAfter - readResult._tcBefore),
                (unsigned long)(readResult._tcBefore - reference._fsTicks),
                (unsigned long)readResult._elapsedCycles, unsigned(readResult._numbered),
                (unsigned long)(readResult._tcAfter - reference._ppsTicks));
        }
        const uint32_t difference = reference._fsTicks - reference._ppsTicks;
        printf("PPS SAMPLE event=%lu ppsTicks=%lu sample=%lu fsTicks=%lu fsAfterPpsTicks=%lu fsAfterPpsNs=%.3f inputAfterPpsNs=%.3f ppsTicks64=%llu fsTicks64=%llu\n",
            (unsigned long)reference._event, (unsigned long)reference._ppsTicks,
            (unsigned long)reference._sample, (unsigned long)reference._fsTicks,
            (unsigned long)difference, difference * 6.25, difference * 6.25 - 2553.0,
            (unsigned long long)reference._ppsTicksExtended, (unsigned long long)reference._fsTicksExtended);
    }
    const bool passed = numbered && samplesExist && !getFailed() && badIntervals == 0 &&
        _late == 0 && _referencesCount >= 59 && _referencesCount <= 61 &&
        _count >= 59 && _count <= 61 && _wraps >= 2;
    printf("PPS WRAPS count=%lu observedSeconds=%.6f\n",
        (unsigned long)_wraps, double(_elapsedTicks) / 160000000.0);
    printf("PPS BOUNDARIES captured=%u failed=%u missing=%u\n",
        boundaries, _boundaryFailures, unsigned(_missing));
    printf("PPS TOTAL captured=%u mapped=%u beforeFirst=%u afterLast=%u late=%u badIntervals=%u overflow=%u numbered=%u\n",
        _count, _referencesCount, _beforeFirst, _count - _nextEvent, _late,
        badIntervals, unsigned(_overflow), unsigned(numbered));
    printf("PPS LIMIT: directly observed reference sample, not necessarily nearest sample; nominal ticks; 2553-ns path correction; no UTC or GPS-validity check; SD load depends on selected mode\n");
    printf("PPS RESULT run=%lu status=%s\n", (unsigned long)run, passed ? "MEASURED" : "FAIL");
}
