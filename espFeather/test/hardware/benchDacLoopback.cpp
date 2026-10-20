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

/** @file benchDacLoopback.cpp
 * @brief Measure the existing bench DAC with Feather's own analog input.
 * Wiring (power off): A1/GPIO25 to A2/GPIO34; disconnect TI coax for the first
 * test. No ADS127 SPI or UART commands are issued. setup leaves outputs off.
 * USB g starts the SAME benchDac timer/code path used in the P4 test. loop skips
 * one second, measures A2 for 300 ms, stops DAC, then prints timestamped values.
 * This checks waveform shape with an independent ADC, not calibrated precision.
 */
#include <Arduino.h>
#include <driver/adc.h>
#include <driver/dac.h>
#include <esp_timer.h>
#include "benchDac.h"

/** @brief One physical A2 measurement; time is microseconds after sampling begins. */
struct loopbackPoint
{
    uint32_t timeUs;
    int raw;
};
static benchDac source;
static loopbackPoint points[320];
static unsigned pointCount = 0;
static bool ready = false, running = false;
static int64_t startedAt = 0, nextReadAt = 0;
static uint32_t missedReadSlots = 0;

/** @brief Stop the timer/DAC before USB output; loop alone owns the sample array.
 * The caller passes clean=true only after the timed window ends. The printed
 * clean flag also requires at least 200 readings and no known DAC error. Missed
 * read slots are reported separately; clean does not require zero misses or
 * mean a faithful sine or correct wiring. Independent ADC readings may include
 * noise/nonlinearity. Print actual timestamps, never pretend sampling was exact.
 */
static void finishMeasurement(bool clean)
{
    source.stop();
    running = false;
    const bool good = clean && !source.failed() && pointCount >= 200;
    Serial.printf("LOOP_BEGIN count=%u clean=%u missedReadSlots=%lu\n", pointCount,
                  unsigned(good), static_cast<unsigned long>(missedReadSlots));
    for (unsigned index = 0; index < pointCount; ++index)
    {
        Serial.printf("LOOP,%lu,%d\n", static_cast<unsigned long>(points[index].timeUs), points[index].raw);
    }
    Serial.println("LOOP_END");
}

/** @brief Arduino boot initializes a stopped DAC source and 12-bit A2 ADC.
 * Attenuation extends the ADC's input range for the nominal 0.75..1.75 V signal;
 * values are raw 0..4095 counts, not voltage calibration. Configuration errors
 * refuse g. A2 is ADC1 channel 6 on Feather ESP32 V2, confirmed by board mapping.
 */
void setup()
{
    dac_output_disable(DAC_CHANNEL_1);
    dac_output_disable(DAC_CHANNEL_2);
    pinMode(A1, INPUT);
    pinMode(A2, INPUT);
    Serial.begin(115200);
    ready = source.initialize() && adc1_config_width(ADC_WIDTH_BIT_12) == ESP_OK &&
        adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_12) == ESP_OK;
    delay(500);
    Serial.println("FEATHER DAC LOOPBACK: A1 -> A2 jumper; TI coax disconnected for first test.");
    Serial.println("Same 10 Hz bench DAC. g=start, s=stop. No ADS127 commands; DAC off at boot.");
    Serial.println(ready ? "LOOP READY" : "LOOP initialization FAILED");
}

/** @brief Arduino loop samples independently while the timer task updates DAC.
 * Read at most once per nominal 1 ms slot, recording actual conversion midpoint
 * time. Skipped slots are counted instead of replaying stale samples. All memory
 * is global, bounded, and owned here; there is no printing during measurement.
 * Stop after the timed window even without a PC reader. USB s aborts immediately.
 */
void loop()
{
    if (Serial.available())
    {
        const char command = static_cast<char>(Serial.read());
        if (command == 'g' && ready && !running)
        {
            pointCount = missedReadSlots = 0;
            if (source.start())
            {
                startedAt = esp_timer_get_time();
                nextReadAt = startedAt + 1000000;
                running = true;
            }
            else { Serial.println("LOOP DAC start FAILED"); }
        }
        else if (command == 's' && running) { finishMeasurement(false); }
    }
    if (running)
    {
        const int64_t now = esp_timer_get_time();
        if (source.failed()) { finishMeasurement(false); }
        else if (now - startedAt >= 1300000) { finishMeasurement(true); }
        else if (now >= nextReadAt)
        {
            missedReadSlots += static_cast<uint32_t>((now - nextReadAt) / 1000);
            nextReadAt += ((now - nextReadAt) / 1000 + 1) * 1000;
            const int64_t before = esp_timer_get_time();
            const int value = adc1_get_raw(ADC1_CHANNEL_6);
            const int64_t after = esp_timer_get_time();
            if (value < 0 || pointCount >= 320) { finishMeasurement(false); }
            else
            {
                points[pointCount++] = {static_cast<uint32_t>((before + after) / 2 - startedAt - 1000000), value};
            }
        }
    }
    delay(1);
}
