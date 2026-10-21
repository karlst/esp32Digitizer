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
 * @file BenchDac.cpp
 * @brief Set the Feather's analog output to a test sine wave, square wave, or fixed level.
 * A DAC converts an eight-bit number to an analog voltage. This code commands
 * GPIO25/A1; it does not measure whether that voltage was produced correctly.
 * Arduino's loop requests start/stop. ESP's timer-service task calls tick()
 * about every 500 microseconds; a mutex lock keeps those callers from changing
 * the same state or output at once. Timer callbacks can still arrive late.
 * Read initialize(), start(), tick(), stop(). Timer callbacks never print or
 * issue ADC SPI commands. Arduino loop owns the ADC; this class owns GPIO25.
 */
#include "BenchDac.h"
#if ADS127_P4_DAC_TEST
#include "benchDacWave.h"
#include <driver/dac.h>

/**
 * @brief Create a stopped timer and lock without enabling analog output.
 * Called once by setup(). Failure refuses future DAC start. The timer fires
 * every nominal 500 us only after start(); task scheduling can delay updates.
 */
bool BenchDac::initialize()
{
    _mutex = xSemaphoreCreateMutex();
    bool retVal = _mutex != nullptr;
    if (retVal)
    {
        esp_timer_create_args_t args = {};
        args.callback = tick;
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "benchDac";
        args.skip_unhandled_events = true;
        retVal = esp_timer_create(&args, &_timer) == ESP_OK;
    }
    if (!retVal && _mutex != nullptr)
    {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
    return retVal;
}

/**
 * @brief Enable DAC channel 1 (A1/GPIO25); the caller must first obtain receiver READY.
 * This class cannot check receiver readiness. The standalone loopback test
 * also calls this without any external receiver.
 * Write codeAt(0) first (sine midpoint, square-wave low level, or constant-test code 135), then start
 * periodic updates. True means driver calls
 * succeeded, not measured voltage. Any failure disables the output again.
 */
bool BenchDac::start()
{
    bool retVal = _mutex != nullptr && _timer != nullptr;
    if (retVal)
    {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _beganAt = _lastAt = esp_timer_get_time();
        _updates = _missed = 0;
        _maxGapUs = 0;
        _broken = false;
        retVal = dac_output_enable(DAC_CHANNEL_1) == ESP_OK &&
                 dac_output_voltage(DAC_CHANNEL_1, benchDacWave::codeAt(0)) == ESP_OK;
        _enabled = retVal;
        xSemaphoreGive(_mutex);
        if (retVal) { retVal = esp_timer_start_periodic(_timer, benchDacWave::updateUs) == ESP_OK; }
    }
    if (!retVal) { stop(); }
    return retVal;
}

/**
 * @brief Write the selected sine, square-wave, or constant code from ESP's timer-service task.
 * Missed counts whole extra nominal intervals in a callback gap, not exact
 * hardware timer losses. A driver error latches broken; loop() requests ADC stop.
 * Pending callbacks after stop see enabled=false and cannot re-enable output.
 */
void BenchDac::tick(void* context)
{
    auto* benchDac = static_cast<BenchDac*>(context);
    xSemaphoreTake(benchDac->_mutex, portMAX_DELAY);
    if (benchDac->_enabled)
    {
        const int64_t now = esp_timer_get_time();
        const uint64_t gap = static_cast<uint64_t>(now - benchDac->_lastAt);
        if (gap > benchDac->_maxGapUs) { benchDac->_maxGapUs = gap; }
        if (gap >= 2 * benchDacWave::updateUs)
        {
            benchDac->_missed += static_cast<uint32_t>(gap / benchDacWave::updateUs - 1);
        }
        benchDac->_lastAt = now;
        const uint8_t code = benchDacWave::codeAt(static_cast<uint64_t>(now - benchDac->_beganAt));
        if (dac_output_voltage(DAC_CHANNEL_1, code) != ESP_OK) { benchDac->_broken = true; }
        ++benchDac->_updates;
    }
    xSemaphoreGive(benchDac->_mutex);
}

/**
 * @brief Stop periodic updates and disable the DAC, then print run diagnostics.
 * Arduino loop calls after requesting ADC stop, including failed handshakes.
 * Stopping a timer alone does not synchronize a callback already running; the
 * mutex plus enabled=false does. Output disabled means undriven, not calibrated
 * zero volts. Check the return code instead of claiming a failed disable worked.
 */
void BenchDac::stop()
{
    if (_timer != nullptr) { esp_timer_stop(_timer); }
    if (_mutex != nullptr) { xSemaphoreTake(_mutex, portMAX_DELAY); }
    _enabled = false;
    const bool off = dac_output_disable(DAC_CHANNEL_1) == ESP_OK;
    if (!off) { _broken = true; }
    const uint32_t count = _updates, gaps = _missed;
    const uint64_t longest = _maxGapUs;
    const bool fault = _broken;
    if (_mutex != nullptr) { xSemaphoreGive(_mutex); }
    Serial.printf("DAC %s updates=%lu missedIntervals=%lu maxGapUs=%llu driverFault=%u\n",
                  off ? "OFF" : "disable FAILED", static_cast<unsigned long>(count),
                  static_cast<unsigned long>(gaps), static_cast<unsigned long long>(longest),
                  static_cast<unsigned>(fault));
}

/** @brief Read the callback's failure latch safely from Arduino loop. */
bool BenchDac::getFailed()
{
    bool retVal = true;
    if (_mutex != nullptr)
    {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        retVal = _broken;
        xSemaphoreGive(_mutex);
    }
    return retVal;
}
#endif
