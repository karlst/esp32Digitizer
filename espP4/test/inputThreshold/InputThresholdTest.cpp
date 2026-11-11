/* Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
 * Read initialize, then captured: startup configures the input and output;
 * MCPWM invokes captured in an ISR (Interrupt Service Routine) on each edge.
 * Core 0 initializes the hardware. Only the callback changes the marker after
 * startup. There are no shared sample buffers, disk writes or per-edge prints.
 * Keep the generator off during wiring/upload; drive GPIO6 only while powered.
 */
#include "InputThresholdTest.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include <cstdio>

/* **
 * initialize - Called once by app_main before the generator is enabled. GPIO23
 * starts LOW and GPIO6 uses the same weak pull-down as PulseDelayCapture.
 * Allocation or setup failure aborts startup through ESP_ERROR_CHECK; a ready
 * message means setup succeeded, not that a valid waveform has been measured.
 */
void InputThresholdTest::initialize()
{
    // Set the output latch LOW before enabling the driver. GPIO23 must be wired
    // only to the scope input, never to another driven signal or supply rail.
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_23, 0));
    gpio_config_t output = {};
    output.pin_bit_mask = 1ULL << 23;
    output.mode = GPIO_MODE_OUTPUT;
    output.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&output));

    // Match the calibration's 160-MHz capture clock and GPIO6 pull-down. The
    // hardware timestamp is not used as a voltage measurement in this test.
    mcpwm_capture_timer_config_t timer = {};
    timer.group_id = 0;
    timer.clk_src = MCPWM_CAPTURE_CLK_SRC_PLL160M;
    timer.resolution_hz = 160000000;
    ESP_ERROR_CHECK(mcpwm_new_capture_timer(&timer, &_timer));
    mcpwm_capture_channel_config_t input = {};
    input.gpio_num = 6;
    input.prescale = 1;
    input.flags.pos_edge = true;
    input.flags.neg_edge = true;
    input.flags.pull_down = true;
    ESP_ERROR_CHECK(mcpwm_new_capture_channel(_timer, &input, &_channel));

    // Register before enabling capture so the first edge has a handler. No
    // extra glitch or hysteresis filter is enabled. Noise near the threshold
    // may cause multiple transitions; do not hide those with software debounce.
    mcpwm_capture_event_callbacks_t callbacks = {};
    callbacks.on_cap = captured;
    ESP_ERROR_CHECK(mcpwm_capture_channel_register_event_callbacks(_channel, &callbacks, this));
    ESP_ERROR_CHECK(mcpwm_capture_timer_enable(_timer));
    ESP_ERROR_CHECK(mcpwm_capture_timer_start(_timer));
    ESP_ERROR_CHECK(mcpwm_capture_channel_enable(_channel));
    _nextReportUs = esp_timer_get_time() + 1000000;
    printf("INPUT_THRESHOLD version=2 ready=1 input=GPIO6 marker=GPIO23 pullDown=1 mode=readLevel\n");
    printf("Marker copies GPIO6 in capture callback; latency remains. Audit mismatches invalidate threshold interpretation.\n");
}

/* **
 * captured - MCPWM calls this interrupt handler after detecting an edge. Read
 * GPIO6 now and copy that digital level to GPIO23. This deliberately replaces
 * the previous use of reported edge polarity. Count disagreement between that
 * polarity and the current input: rapid chatter during interrupt latency can
 * cause disagreement, so it does not by itself prove a capture-driver defect.
 * Return false because no task was awakened. The marker still includes input
 * read, interrupt and output latency, and is not an exact edge timestamp.
 * context points to the instance registered in initialize. event describes
 * the captured transition; its timestamp is not used to drive the marker.
 */
bool InputThresholdTest::captured(mcpwm_cap_channel_handle_t,
    const mcpwm_capture_event_data_t* event, void* context)
{
    auto& inputThresholdTest = *static_cast<InputThresholdTest*>(context);

    // Keep the audit from inspecting a partly updated software record. Only
    // this callback drives GPIO23 after startup; the audit never repairs it.
    portENTER_CRITICAL_ISR(&inputThresholdTest._mux);
    const int level = gpio_get_level(GPIO_NUM_6);
    gpio_set_level(GPIO_NUM_23, level);
    inputThresholdTest._markerLevel = level;
    ++inputThresholdTest._callbacks;
    if (level != (event->cap_edge == MCPWM_CAP_EDGE_POS ? 1 : 0))
    {
        ++inputThresholdTest._edgeDisagreements;
    }
    portEXIT_CRITICAL_ISR(&inputThresholdTest._mux);
    return false;
}

/* **
 * poll - The main task calls this about once per scheduler tick (1 ms here).
 * Compare two input reads with the last commanded marker level under a lock.
 * A stable pair that disagrees flags an output that has not caught up; an edge
 * arriving during the audit can also cause this, so it is not proof of a missed
 * interrupt. No marker correction or debounce is applied. Print cumulative
 * counters once per second outside the lock; printing must not block capture.
 */
void InputThresholdTest::poll()
{
    uint32_t callbacks = 0;
    uint32_t edgeDisagreements = 0;
    uint32_t audits = 0;
    uint32_t auditDisagreements = 0;
    int inputLevel = 0;
    int markerLevel = 0;

    // The lock serializes this task with the ISR, not with the physical input.
    // Two matching reads can still miss brief chatter between those reads.
    portENTER_CRITICAL(&_mux);
    inputLevel = gpio_get_level(GPIO_NUM_6);
    // This is the level commanded by the callback, not a readback of the
    // physical GPIO23 voltage. The scope must verify the actual marker output.
    markerLevel = _markerLevel;
    const int secondLevel = gpio_get_level(GPIO_NUM_6);
    if (inputLevel == secondLevel)
    {
        ++_audits;
        if (inputLevel != markerLevel)
        {
            ++_auditDisagreements;
        }
    }
    callbacks = _callbacks;
    edgeDisagreements = _edgeDisagreements;
    audits = _audits;
    auditDisagreements = _auditDisagreements;
    portEXIT_CRITICAL(&_mux);

    // esp_timer supplies microseconds for report scheduling only. These counts
    // help diagnose the marker; they neither measure voltage nor bound latency.
    const int64_t nowUs = esp_timer_get_time();
    if (nowUs >= _nextReportUs)
    {
        _nextReportUs = nowUs + 1000000;
        printf("THRESHOLD callbacks=%lu edgeDisagreements=%lu audits=%lu auditDisagreements=%lu input=%d marker=%d\n",
            (unsigned long)callbacks, (unsigned long)edgeDisagreements,
            (unsigned long)audits, (unsigned long)auditDisagreements,
            inputLevel, markerLevel);
    }
}
