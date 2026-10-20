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

/** @file inputSnapshot.cpp
 * @brief Measure the test voltage through Feather A2 while the external ADC captures it.
 * This is a second ADC observing the same wire, not a copy of the values sent
 * to the DAC. It can have its own noise and calibration error. poll() runs in
 * the Feather control loop; report() prints stored results after local sampling
 * stops. No USB output occurs in poll().
 * Read begin(), poll(), report(). One second of settling and a 300 ms observation
 * window match the P4 RAM collector. Their clocks are independent: PC analysis
 * may fit a small relative time shift for sine comparison. Constant-voltage
 * reports do not align clocks or fit gain. report() exports calibration only
 * after capture stops, leaving acquisition timing unchanged.
 */
#include "inputSnapshot.h"
#if ADS127_COMPARE_INPUT
#include <driver/adc.h>
#include <esp_timer.h>
#include <esp_adc_cal.h>

/** @brief setup configures ADC1 range/width without enabling DAC or ADS127.
 * True confirms configuration calls, not jumper presence or measured accuracy.
 */
bool inputSnapshot::initialize()
{
    pinMode(A2, INPUT);
    const bool retVal = adc1_config_width(ADC_WIDTH_BIT_12) == ESP_OK &&
        adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_12) == ESP_OK;
    return retVal;
}

/** @brief Discard the previous record before accepting a new run, while idle. */
void inputSnapshot::reset()
{
    collecting = broken = false;
    count = missed = maxGapUs = 0;
    origin = nextRead = lastRead = 0;
}

/** @brief Called immediately after confirmed ADC START; establish a local origin.
 * The origin is software observation of the command echo, not an ADC sample
 * edge. The exported timestamp and PC shift fit make that distinction explicit.
 */
void inputSnapshot::begin()
{
    reset();
    origin = esp_timer_get_time();
    nextRead = origin + 1000000;
    collecting = true;
}

/** @brief loop collects a nominal 500 us A2 burst once per millisecond slot.
 * adc1_get_raw performs each conversion; an arithmetic mean, extrema, count and
 * actual burst duration are retained. Calls are not uniformly timed, so this is
 * an approximation to P4's uniform half-millisecond average, not identical
 * filtering. Late slots are skipped and counted. A conversion already in flight
 * may extend beyond the nominal deadline; preemption also extends duration.
 * The timer task can still update DAC during a burst. No locks/USB output here.
 * This extra measurement work is diagnostic-only, not production qualification.
 */
void inputSnapshot::poll()
{
    if (collecting)
    {
        const int64_t now = esp_timer_get_time();
        if (now - origin >= 1299500) { collecting = false; }
        else if (now >= nextRead)
        {
            missed += static_cast<uint32_t>((now - nextRead) / 1000);
            nextRead += ((now - nextRead) / 1000 + 1) * 1000;
            const int64_t before = esp_timer_get_time();
            uint32_t sum = 0, reads = 0;
            int minimum = 4095, maximum = 0;
            // Bound both elapsed time and count. 4095*4096 fits in uint32_t;
            // the count limit also bounds work if a clock source malfunctions.
            do
            {
                const int raw = adc1_get_raw(ADC1_CHANNEL_6);
                if (raw < 0 || raw > 4095) { broken = true; }
                else
                {
                    sum += static_cast<uint32_t>(raw);
                    ++reads;
                    if (raw < minimum) { minimum = raw; }
                    if (raw > maximum) { maximum = raw; }
                }
            } while (!broken && reads < 4096 && esp_timer_get_time() - before < 500);
            const int64_t after = esp_timer_get_time();
            const int64_t at = (before + after) / 2;
            if (broken || reads < 2 || count >= 320)
            {
                broken = true;
                collecting = false;
            }
            else
            {
                if (lastRead != 0 && at - lastRead > maxGapUs)
                {
                    maxGapUs = static_cast<uint32_t>(at - lastRead);
                }
                lastRead = at;
                points[count++] = {static_cast<uint32_t>(at - origin),
                    static_cast<uint32_t>(after - before), reads, sum, minimum, maximum};
            }
        }
    }
}

/** @brief Tell loop to poll frequently only during the 300 ms diagnostic window.
 * A full operating-system scheduling tick after every 500 us burst can miss
 * 1 ms slots. Outside
 * this short interval loop retains its ordinary sleep. Never used in other builds.
 */
bool inputSnapshot::sampling() const
{
    return collecting && esp_timer_get_time() - origin >= 1000000;
}

/** @brief loop reads its own latched ADC-driver/capacity failure state. */
bool inputSnapshot::failed() const
{
    return broken;
}

/** @brief Export the matching run after stop, preserving actual sample times.
 * At least 200 measurements spanning 250 ms are required for clean collection;
 * P4 may request stop just before this window ends. This flag does not assess
 * waveform shape. The PC also validates matching run IDs, row counts and order.
 */
void inputSnapshot::report(unsigned long run, bool hardwareClean)
{
    collecting = false;

    // After acquisition has stopped, ask the SDK for this ADC's count-to-mV
    // mapping. Factory eFuse values are permanent calibration data in the chip;
    // when absent the SDK uses an explicitly labelled nominal 1100 mV reference.
    // Export a small lookup table rather than assuming a 3.3 V / 4095 scale.
    // The PC interpolates this integer-mV table; this is approximate calibration,
    // not proof of absolute accuracy. No extra conversions occur during capture.
    esp_adc_cal_characteristics_t calibration = {};
    const esp_adc_cal_value_t calibrationType = esp_adc_cal_characterize(
        ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, 1100, &calibration);
    Serial.printf("INPUT_CAL run=%lu source=%u defaultVrefMv=1100\n",
                  run, unsigned(calibrationType));
    for (unsigned raw = 0; raw < 4096; raw += 32)
    {
        Serial.printf("CAL,%u,%lu\n", raw, static_cast<unsigned long>(
            esp_adc_cal_raw_to_voltage(raw, &calibration)));
    }
    Serial.printf("CAL,4095,%lu\n", static_cast<unsigned long>(
        esp_adc_cal_raw_to_voltage(4095, &calibration)));
    const bool clean = hardwareClean && !broken && count >= 200 &&
        points[count - 1].timeUs - points[0].timeUs >= 250000;
    Serial.printf("INPUT_BEGIN run=%lu count=%u clean=%u missed=%lu maxGapUs=%lu mode=burst\n",
                  run, count, unsigned(clean), static_cast<unsigned long>(missed),
                  static_cast<unsigned long>(maxGapUs));
    for (unsigned index = 0; index < count; ++index)
    {
        const auto& point = points[index];
        Serial.printf("INPUT,%u,%lu,%.6f,%d,%d,%lu,%lu\n", index,
                      static_cast<unsigned long>(point.timeUs), double(point.sum) / point.reads,
                      point.minimum, point.maximum, static_cast<unsigned long>(point.reads),
                      static_cast<unsigned long>(point.durationUs));
    }
    Serial.printf("INPUT_END run=%lu\n", run);
}
#endif
