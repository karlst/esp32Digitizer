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
 * @file benchDac.cpp
 * @brief Task-timed sine or constant DAC source with explicit start and stop.
 * Read initialize(), start(), tick(), stop(). Timer callbacks never print or
 * issue ADC SPI commands. Arduino loop owns the ADC; this class owns GPIO25.
 */
#include "benchDac.h"
#if ADS127_P4_DAC_TEST
#include "benchDacWave.h"
#include <driver/dac.h>

/**
 * @brief Create a stopped timer and lock without enabling analog output.
 * Called once by setup(). Failure refuses future DAC start. The timer fires
 * every nominal 500 us only after start(); task scheduling can delay updates.
 */
bool benchDac::initialize()
{
    mutex = xSemaphoreCreateMutex();
    bool retVal = mutex != nullptr;
    if (retVal)
    {
        esp_timer_create_args_t args = {};
        args.callback = tick;
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "benchDac";
        args.skip_unhandled_events = true;
        retVal = esp_timer_create(&args, &timer) == ESP_OK;
    }
    if (!retVal && mutex != nullptr)
    {
        vSemaphoreDelete(mutex);
        mutex = nullptr;
    }
    return retVal;
}

/**
 * @brief Enable DAC channel 1 (A1/GPIO25) only after matching receiver READY.
 * Write codeAt(0) first (sine midpoint, or constant-test code 135), then start
 * periodic updates. True means driver calls
 * succeeded, not measured voltage. Any failure disables the output again.
 */
bool benchDac::start()
{
    bool retVal = mutex != nullptr && timer != nullptr;
    if (retVal)
    {
        xSemaphoreTake(mutex, portMAX_DELAY);
        beganAt = lastAt = esp_timer_get_time();
        updates = missed = 0;
        maxGapUs = 0;
        broken = false;
        retVal = dac_output_enable(DAC_CHANNEL_1) == ESP_OK &&
                 dac_output_voltage(DAC_CHANNEL_1, benchDacWave::codeAt(0)) == ESP_OK;
        enabled = retVal;
        xSemaphoreGive(mutex);
        if (retVal) { retVal = esp_timer_start_periodic(timer, benchDacWave::updateUs) == ESP_OK; }
    }
    if (!retVal) { stop(); }
    return retVal;
}

/**
 * @brief Write the selected sine/constant code from ESP's timer-service task.
 * Missed counts whole extra nominal intervals in a callback gap, not exact
 * hardware timer losses. A driver error latches broken; loop() requests ADC stop.
 * Pending callbacks after stop see enabled=false and cannot re-enable output.
 */
void benchDac::tick(void* context)
{
    auto* source = static_cast<benchDac*>(context);
    xSemaphoreTake(source->mutex, portMAX_DELAY);
    if (source->enabled)
    {
        const int64_t now = esp_timer_get_time();
        const uint64_t gap = static_cast<uint64_t>(now - source->lastAt);
        if (gap > source->maxGapUs) { source->maxGapUs = gap; }
        if (gap >= 2 * benchDacWave::updateUs)
        {
            source->missed += static_cast<uint32_t>(gap / benchDacWave::updateUs - 1);
        }
        source->lastAt = now;
        const uint8_t code = benchDacWave::codeAt(static_cast<uint64_t>(now - source->beganAt));
        if (dac_output_voltage(DAC_CHANNEL_1, code) != ESP_OK) { source->broken = true; }
        ++source->updates;
    }
    xSemaphoreGive(source->mutex);
}

/**
 * @brief Stop periodic updates and disable the DAC, then print run diagnostics.
 * Arduino loop calls after requesting ADC stop, including failed handshakes.
 * Stopping a timer alone does not synchronize a callback already running; the
 * mutex plus enabled=false does. Output disabled means undriven, not calibrated
 * zero volts. Check the return code instead of claiming a failed disable worked.
 */
void benchDac::stop()
{
    if (timer != nullptr) { esp_timer_stop(timer); }
    if (mutex != nullptr) { xSemaphoreTake(mutex, portMAX_DELAY); }
    enabled = false;
    const bool off = dac_output_disable(DAC_CHANNEL_1) == ESP_OK;
    if (!off) { broken = true; }
    const uint32_t count = updates, gaps = missed;
    const uint64_t longest = maxGapUs;
    const bool fault = broken;
    if (mutex != nullptr) { xSemaphoreGive(mutex); }
    Serial.printf("DAC %s updates=%lu missedIntervals=%lu maxGapUs=%llu driverFault=%u\n",
                  off ? "OFF" : "disable FAILED", static_cast<unsigned long>(count),
                  static_cast<unsigned long>(gaps), static_cast<unsigned long long>(longest),
                  static_cast<unsigned>(fault));
}

/** @brief Read the callback's failure latch safely from Arduino loop. */
bool benchDac::failed()
{
    bool retVal = true;
    if (mutex != nullptr)
    {
        xSemaphoreTake(mutex, portMAX_DELAY);
        retVal = broken;
        xSemaphoreGive(mutex);
    }
    return retVal;
}
#endif
