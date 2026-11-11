/* Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
 * Capture the direct test-pulse branch using the same timer as FSYNC.
 */
#pragma once
#include "driver/mcpwm_cap.h"
#include "freertos/FreeRTOS.h"
#include "FsyncAssociationAnalysis.h"

/**
 * Save hardware timestamps of a 1-kHz or 10-kHz square wave on GPIO6. The capture
 * interrupt copies already latched times into internal RAM; interrupt arrival
 * time is not used. Core 0 arms/stops capture and prints only after ADC decoding
 * has drained. The shared lock protects the interrupt's bounded event array.
 */
class PulseDelayCapture
{
public:
    void initialize(mcpwm_cap_timer_handle_t timer);
    void start();
    void stop();
    void report(uint32_t run, bool associated, const uint32_t* words, uint32_t count,
        const FsyncAssociationAnalysis::Anchor* anchors,
        const FsyncAssociationAnalysis::Result& result);

private:
    /** One hardware timer reading and the direction of the voltage transition. */
    struct Event
    {
        // Timer count latched when the input circuitry detects the transition;
        // this includes the input path's threshold/detection effects. It is not
        // the time software services the interrupt. true means LOW to HIGH.
        uint32_t _ticks = 0;
        bool _rising = false;
    };
    static bool captured(mcpwm_cap_channel_handle_t channel,
        const mcpwm_capture_event_data_t* event, void* context);
    mcpwm_cap_channel_handle_t _channel = nullptr;
    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
    // 4096 edges allow about 205 ms at 10 kHz, including startup and stopping.
    // This static object's array is internal RAM usable by the ISR; ADC words
    // are held separately in FsyncAssociationTest's external PSRAM allocation.
    static constexpr size_t _eventCapacity = 4096;
    Event _events[_eventCapacity] = {};
    size_t _count = 0;
    bool _armed = false;
    bool _overflow = false;
};
