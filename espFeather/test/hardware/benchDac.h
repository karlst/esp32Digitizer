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
 * @file BenchDac.h
 * @brief Opt-in Feather DAC source for the external-input P4 recording test.
 * This is separate from the web application's DAC/ADC loopback service: no
 * Wi-Fi, browser, local analog sampling, or graph allocation is required here.
 */
#pragma once
#if ADS127_P4_DAC_TEST
#include <Arduino.h>
#include <esp_timer.h>
#include <freertos/semphr.h>

/**
 * @brief Prevent timer writes to A1 from overlapping the loop's start/stop calls.
 * ESP_TIMER_TASK invokes tick() in a shared task, NOT an interrupt. A mutex
 * prevents stop() from disabling the DAC while a callback writes a new code.
 * No DAC output is enabled by construction or initialize(). Objects live for
 * the firmware lifetime; timer/mutex storage is reused between finite runs.
 */
class BenchDac
{
public:
    bool initialize();
    bool start();
    void stop();
    bool getFailed();
private:
    static void tick(void* context);
    SemaphoreHandle_t _mutex = nullptr;
    esp_timer_handle_t _timer = nullptr;
    bool _enabled = false, _broken = false;
    int64_t _beganAt = 0, _lastAt = 0;
    uint32_t _updates = 0, _missed = 0;
    uint64_t _maxGapUs = 0;
};
#endif
