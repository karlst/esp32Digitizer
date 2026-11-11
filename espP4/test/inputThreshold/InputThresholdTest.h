/* Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
 * Standalone scope-assisted GPIO6 threshold test. GPIO23 is the marker output.
 */
#pragma once

#include "driver/mcpwm_cap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

/**
 * InputThresholdTest marks GPIO6 transitions on GPIO23 for oscilloscope viewing.
 * MCPWM (Motor Control PWM) supplies hardware input capture, as in the original
 * calibration. A rising edge means LOW becoming HIGH; a falling edge is the
 * reverse. The interrupt callback changes GPIO23 after detection, not at the
 * exact captured instant. With a slow ramp, that delay causes a small voltage
 * error which must be checked by changing ramp speed. This firmware neither
 * measures voltage nor establishes a numerical uncertainty bound.
 */
class InputThresholdTest
{
public:
    void initialize();
    void poll();

private:
    static bool captured(mcpwm_cap_channel_handle_t channel,
        const mcpwm_capture_event_data_t* event, void* context);

    mcpwm_cap_timer_handle_t _timer = nullptr;
    mcpwm_cap_channel_handle_t _channel = nullptr;

    // Core 0's interrupt and polling task share these under the same lock.
    // Counters describe software observations, not guaranteed physical edges.
    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
    uint32_t _callbacks = 0;
    uint32_t _edgeDisagreements = 0;
    uint32_t _audits = 0;
    uint32_t _auditDisagreements = 0;
    int _markerLevel = 0;
    int64_t _nextReportUs = 0;
};
