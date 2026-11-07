/* Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
 * Measure relative input timing: the SAME generator signal must reach GPIO1
 * and GPIO6, with the ADC FSYNC wire removed from GPIO1. "Skew" here means
 * GPIO1's recorded time minus GPIO6's recorded time for the same transition.
 * The PC pairs these observations in inputSkewReport.py. Common latency in both
 * paths cancels, so this test cannot measure absolute input-detection latency.
 *
 * Read initialize -> poll -> start -> captured -> finish. Capture is command-only.
 * app_main calls poll on Core 0. Each channel's ISR (Interrupt Service Routine)
 * appends to its own array; a shared lock protects reset/stop against those
 * callbacks. Output is deferred until both channels stop, so printing cannot
 * hold up event collection. A task is a scheduled execution thread; the
 * hardware timer continues independently of when that task or ISR runs.
 */
#include "InputSkewTest.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

/* **
 * initialize - Core 0 allocates the same capture timer and input channels used
 * in the pulse-delay measurement. GPIO1 has no pull resistor; GPIO6 has the
 * same weak pull-down as the direct-pulse test. Neither pin is ever an output.
 * Nonblocking console reads let the main loop finish a capture without waiting
 * for a key. Allocation errors abort initialization rather than giving a result.
 */
void InputSkewTest::initialize()
{
    const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);

    // Preserve existing console flags and add O_NONBLOCK: read must return when
    // no key is waiting, so poll can still notice the 100-ms capture deadline.
    ESP_ERROR_CHECK(flags >= 0 && fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) == 0 ? ESP_OK : ESP_FAIL);
    mcpwm_capture_timer_config_t config = {};

    // MCPWM provides input capture: a transition copies the running counter
    // into a register. Both channels use this single 160-MHz (6.25-ns) clock.
    config.group_id = 0;
    config.clk_src = MCPWM_CAPTURE_CLK_SRC_PLL160M;
    config.resolution_hz = 160000000;
    ESP_ERROR_CHECK(mcpwm_new_capture_timer(&config, &_timer));
    uint32_t resolution = 0;
    ESP_ERROR_CHECK(mcpwm_capture_timer_get_resolution(_timer, &resolution));
    ESP_ERROR_CHECK(resolution == 160000000 ? ESP_OK : ESP_ERR_INVALID_STATE);

    // Allocation order matches FSYNC on channel zero and direct pulse on one.
    // Both polarities are collected to expose threshold-dependent differences.
    for (size_t iChannel = 0; iChannel < 2; ++iChannel)
    {
        auto& channel = _channels[iChannel];
        channel._owner = this;
        channel._gpio = iChannel == 0 ? 1 : 6;
        mcpwm_capture_channel_config_t input = {};
        input.gpio_num = channel._gpio;
        input.prescale = 1;
        input.flags.pos_edge = true;
        input.flags.neg_edge = true;
        input.flags.pull_down = channel._gpio == 6;
        ESP_ERROR_CHECK(mcpwm_new_capture_channel(_timer, &input, &channel._handle));
        mcpwm_capture_event_callbacks_t callbacks = {};
        callbacks.on_cap = captured;

        // Passing &channel as callback context identifies the array to append
        // to without inferring the input pin from interrupt arrival order.
        ESP_ERROR_CHECK(mcpwm_capture_channel_register_event_callbacks(channel._handle, &callbacks, &channel));
    }
    ESP_ERROR_CHECK(gpio_set_pull_mode(GPIO_NUM_1, GPIO_FLOATING));
    ESP_ERROR_CHECK(mcpwm_capture_timer_enable(_timer));
    ESP_ERROR_CHECK(mcpwm_capture_timer_start(_timer));
    printf("INPUT_SKEW_P4 version=1 ready=1 hz=160000000 gpioA=1 gpioB=6\n");
}

/* **
 * start - Core 0 clears previous events and arms both inputs for 100 ms.
 * Enabling channels is sequential, so a boundary edge can reach just one.
 * The PC may trim one unmatched edge at each boundary; internal gaps still fail.
 * No per-edge timestamp is made here, and the common timer is not reset.
 */
void InputSkewTest::start()
{
    portENTER_CRITICAL(&_mux);
    for (auto& channel : _channels)
    {
        channel._count = 0;
        channel._overflow = false;
    }
    _armed = true;
    portEXIT_CRITICAL(&_mux);
    ++_run;
    _running = true;
    _startedAt = esp_timer_get_time();

    // This microsecond clock determines capture duration only. Individual edge
    // times come from the hardware capture timer and use different units.
    for (auto& channel : _channels)
    {
        ESP_ERROR_CHECK(mcpwm_capture_channel_enable(channel._handle));
    }
}

/* **
 * captured - The driver invokes this Interrupt Service Routine (ISR) for either
 * input. Hardware already latched cap_value at the voltage transition; the
 * order and latency of the two callbacks cannot change those saved times.
 * The shared lock keeps stop/reset from racing this bounded array write.
 * No printing, dynamic allocation or task notification occurs in the callback.
 * False tells the driver that no waiting task needs an immediate scheduler
 * handoff. Overflow is latched until the next start and invalidates this run.
 */
bool InputSkewTest::captured(mcpwm_cap_channel_handle_t handle,
    const mcpwm_capture_event_data_t* event, void* context)
{
    (void)handle;
    auto& channel = *static_cast<Channel*>(context);
    auto& inputSkewTest = *channel._owner;
    portENTER_CRITICAL_ISR(&inputSkewTest._mux);
    if (inputSkewTest._armed)
    {
        if (channel._count < 512)
        {
            channel._events[channel._count++] = {event->cap_value, event->cap_edge == MCPWM_CAP_EDGE_POS};
        }
        else
        {
            channel._overflow = true;
        }
    }
    portEXIT_CRITICAL_ISR(&inputSkewTest._mux);
    return false;
}

/* **
 * finish - Core 0 stops event writes before exporting both raw timestamp arrays.
 * A pending interrupt sees armed=false and cannot alter exported data. READY
 * only means collection completed without overflow; the PC validates pairing,
 * pulse rate and counts. No tolerance for timing accuracy is assumed here.
 */
void InputSkewTest::finish()
{
    portENTER_CRITICAL(&_mux);
    _armed = false;
    portEXIT_CRITICAL(&_mux);
    for (auto& channel : _channels)
    {
        ESP_ERROR_CHECK(mcpwm_capture_channel_disable(channel._handle));
    }
    for (const auto& channel : _channels)
    {
        // Print complete streams separately. Array index is local to each pin;
        // it is not proof that equal indexes observed the same physical edge.
        for (size_t iEvent = 0; iEvent < channel._count; ++iEvent)
        {
            const auto& event = channel._events[iEvent];
            printf("SKEW EDGE run=%lu gpio=%d index=%u ticks=%lu rising=%u\n",
                (unsigned long)_run, channel._gpio, unsigned(iEvent),
                (unsigned long)event._ticks, unsigned(event._rising));
        }
    }
    const bool overflow = _channels[0]._overflow || _channels[1]._overflow;
    printf("SKEW RESULT run=%lu status=%s count1=%u count6=%u overflow=%u\n",
        (unsigned long)_run, overflow ? "FAIL" : "READY",
        unsigned(_channels[0]._count), unsigned(_channels[1]._count), unsigned(overflow));
    _running = false;
}

/* **
 * poll - Core 0 services h (identity) and g (one finite capture) from USB serial.
 * A repeated g during capture is refused. The caller yields a scheduler tick;
 * elapsed-time stopping is coarse, but individual edge timestamps are hardware.
 */
void InputSkewTest::poll()
{
    char key = 0;
    if (read(STDIN_FILENO, &key, 1) == 1)
    {
        if (key == 'h')
        {
            printf("INPUT_SKEW_P4 version=1 ready=%u hz=160000000 gpioA=1 gpioB=6\n", unsigned(!_running));
        }
        else if (key == 'g')
        {
            if (_running)
            {
                printf("SKEW rejected: capture already running\n");
            }
            else
            {
                start();
            }
        }
    }
    if (_running && esp_timer_get_time() - _startedAt >= 100000)
    {
        finish();
    }
}
