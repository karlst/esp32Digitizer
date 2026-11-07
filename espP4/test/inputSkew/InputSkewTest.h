/* Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
 * Standalone same-pulse comparison of P4 GPIO1 and GPIO6.
 */
#pragma once
#include "driver/mcpwm_cap.h"
#include "freertos/FreeRTOS.h"
#include <cstddef>

/**
 * Hardware timestamps two copies of the same generator pulse. Both channels
 * use one 160-MHz timer. The interrupt only copies latched times into bounded
 * internal RAM; Core 0 stops both channels before reporting. No ADC, UART link
 * to Feather, sample decoder or SD service is constructed by this test.
 */
class InputSkewTest
{
public:
    void initialize();
    void poll();

private:
    /** One input transition: timestamp in timer ticks, direction HIGH or LOW. */
    struct Event
    {
        uint32_t _ticks = 0;
        bool _rising = false;
    };
    /** Storage for one pin, owned by the interrupt until capture stops. */
    struct Channel
    {
        // Callback context points here. owner supplies the shared lock and
        // armed flag; gpio identifies the pin in the eventual console report.
        InputSkewTest* _owner = nullptr;
        int _gpio = 0;
        mcpwm_cap_channel_handle_t _handle = nullptr;
        Event _events[512] = {};
        // Next free event slot, not a count of full square-wave cycles.
        size_t _count = 0;
        bool _overflow = false;
    };
    static bool captured(mcpwm_cap_channel_handle_t handle,
        const mcpwm_capture_event_data_t* event, void* context);
    void start();
    void finish();
    mcpwm_cap_timer_handle_t _timer = nullptr;
    Channel _channels[2] = {};
    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
    // Core 0 owns run numbering and the coarse stop deadline (microseconds).
    // armed is shared with ISRs under the lock; running controls console status.
    uint32_t _run = 0;
    int64_t _startedAt = 0;
    bool _armed = false;
    bool _running = false;
};
