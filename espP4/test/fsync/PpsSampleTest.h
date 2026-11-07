/* Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
 * Hardware PPS timestamps connected to numbered FSYNC observations.
 */
#pragma once
#include "driver/mcpwm_cap.h"
#include "freertos/FreeRTOS.h"
#include <cstdint>
#include "FsyncBoundaryCapture.h"
#include "TimedBoundary.h"

/**
 * GPIO6 captures rising PPS edges (low-to-high transitions) on FSYNC's existing
 * 160-MHz timer. An ISR (Interrupt Service Routine) copies hardware timestamps
 * into bounded internal RAM. Core 0 observe calls
 * attach each pulse to a later, directly captured FSYNC with a calculated sample
 * number. The reference is usually within one millisecond, not necessarily the
 * very next ADC sample. No inferred FSYNC edge or UTC second is invented.
 */
class PpsSampleTest
{
public:
    void initialize(mcpwm_cap_timer_handle_t timer);
    void start();
    void stop();
    void observe(uint32_t pulse, uint32_t ticks);
    void report(uint32_t run, bool numbered, uint32_t decodedWords) const;
    bool getFailed() const;
    bool getBoundary(unsigned iBoundary, TimedBoundary& boundary) const;

private:
    /** One PPS edge and a later observed FSYNC belonging to a known sample. */
    struct Reference
    {
        uint32_t _event;
        uint32_t _ppsTicks;
        uint32_t _fsTicks;
        uint32_t _sample;
        uint64_t _ppsTicksExtended;
        uint64_t _fsTicksExtended;
        uint64_t _tcFsBefore = 0, _tcFsAfter = 0;
        uint32_t _firstSample = 0;
        bool _boundaryValid = false;
        FsyncBoundaryCapture::ReadResult _readResult;
    };
    static bool captured(mcpwm_cap_channel_handle_t channel,
        const mcpwm_capture_event_data_t* event, void* context);
    mcpwm_cap_channel_handle_t _channel = nullptr;
    mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
    FsyncBoundaryCapture _fsyncBoundaryCapture;
    static constexpr unsigned _capacity = 80;
    uint32_t _events[_capacity] = {};
    unsigned _count = 0;
    bool _armed = false, _overflow = false;
    // Core 0 alone owns the following fields. Reports run after ISR shutdown.
    Reference _references[_capacity] = {};
    unsigned _referencesCount = 0, _nextEvent = 0, _beforeFirst = 0, _late = 0;
    uint32_t _firstTicks = 0, _previousTicks = 0, _wraps = 0;
    uint64_t _elapsedTicks = 0;
    bool _haveFirst = false;
    unsigned _boundaryFailures = 0, _badIntervals = 0;
    bool _missing = false;
};
