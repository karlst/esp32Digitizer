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

/** @file InputSnapshot.cpp
 * @brief Measure the test voltage through Feather A2 while the external ADC captures it.
 * This is a second ADC observing the same wire, not a copy of the values sent
 * to the DAC. It can have its own noise and calibration error. poll() runs in
 * the Feather control loop; report() prints stored results after local sampling
 * stops. No USB output occurs in poll().
 * The SD smoke build instead observes 300 bursts spaced 100 ms apart across
 * 30 seconds. A2 samples at each bin center; P4 averages a full disk-data bin.
 * Read begin(), poll(), report(). For RAM comparison, one second of settling and a 300 ms observation
 * window match the P4 RAM collector. Their clocks are independent: PC analysis
 * may fit a small relative time shift for sine comparison. Constant-voltage
 * reports do not align clocks or fit gain. report() exports calibration only
 * after capture stops, leaving acquisition timing unchanged.
 */
#include "InputSnapshot.h"
#if ADS127_COMPARE_INPUT
#include <driver/adc.h>
#include <esp_timer.h>
#include <esp_adc_cal.h>

// Smoke measurements cover the entire 30-second recording: a short burst at
// each 100-ms bin center. The ordinary RAM comparison keeps its original window.
#if ADS127_HARDWARE_SMOKE
static constexpr int64_t firstReadUs = 50000;
static constexpr int64_t readIntervalUs = 100000;
static constexpr int64_t endReadUs = 30000000;
#else
static constexpr int64_t firstReadUs = 1000000;
static constexpr int64_t readIntervalUs = 1000;
static constexpr int64_t endReadUs = 1299500;
#endif

/** @brief setup configures ADC1 range/width without enabling DAC or ADS127.
 * True confirms configuration calls, not jumper presence or measured accuracy.
 */
bool InputSnapshot::initialize()
{
    pinMode(A2, INPUT);
    const bool retVal = adc1_config_width(ADC_WIDTH_BIT_12) == ESP_OK &&
        adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_12) == ESP_OK;
    return retVal;
}

/** @brief Discard the previous record before accepting a new run, while idle. */
void InputSnapshot::reset()
{
    _collecting = _broken = false;
    _count = _missed = _maxGapUs = 0;
    _origin = _nextRead = _lastRead = 0;
}

/** @brief Called immediately after confirmed ADC START; establish a local origin.
 * The origin is software observation of the command echo, not an ADC sample
 * edge. The exported timestamp and PC shift fit make that distinction explicit.
 */
void InputSnapshot::begin()
{
    reset();
    _origin = esp_timer_get_time();
    _nextRead = _origin + firstReadUs;
    _collecting = true;
}

/** @brief loop collects a 500 us A2 burst per slot (1 ms RAM, 100 ms SD smoke).
 * adc1_get_raw performs each conversion; an arithmetic mean, extrema, count and
 * actual burst duration are retained. Calls are not uniformly timed. RAM mode
 * approximates P4's half-millisecond average; smoke mode samples the center of
 * a 100-ms disk average. Neither mode uses identical filtering on both boards. Late slots are skipped and counted. A conversion already in flight
 * may extend beyond the nominal deadline; preemption also extends duration.
 * The timer task can still update DAC during a burst. No locks/USB output here.
 * This extra measurement work is diagnostic-only, not production qualification.
 */
void InputSnapshot::poll()
{
    if (_collecting)
    {
        const int64_t now = esp_timer_get_time();
        if (now - _origin >= endReadUs) { _collecting = false; }
        else if (now >= _nextRead)
        {
            _missed += static_cast<uint32_t>((now - _nextRead) / readIntervalUs);
            _nextRead += ((now - _nextRead) / readIntervalUs + 1) * readIntervalUs;
            const int64_t before = esp_timer_get_time();
            uint32_t sum = 0, reads = 0;
            int minimum = 4095, maximum = 0;
            // Bound both elapsed time and count. 4095*4096 fits in uint32_t;
            // the count limit also bounds work if a clock source malfunctions.
            do
            {
                const int raw = adc1_get_raw(ADC1_CHANNEL_6);
                if (raw < 0 || raw > 4095) { _broken = true; }
                else
                {
                    sum += static_cast<uint32_t>(raw);
                    ++reads;
                    if (raw < minimum) { minimum = raw; }
                    if (raw > maximum) { maximum = raw; }
                }
            } while (!_broken && reads < 4096 && esp_timer_get_time() - before < 500);
            const int64_t after = esp_timer_get_time();
            const int64_t at = (before + after) / 2;
            if (_broken || reads < 2 || _count >= 320)
            {
                _broken = true;
                _collecting = false;
            }
            else
            {
                if (_lastRead != 0 && at - _lastRead > _maxGapUs)
                {
                    _maxGapUs = static_cast<uint32_t>(at - _lastRead);
                }
                _lastRead = at;
                _points[_count++] = {static_cast<uint32_t>(at - _origin),
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
bool InputSnapshot::sampling() const
{
    // Sleeping between the sparse smoke bursts does not lose 100-ms slots.
#if ADS127_HARDWARE_SMOKE
    return false;
#else
    return _collecting && esp_timer_get_time() - _origin >= firstReadUs;
#endif
}

/** @brief loop reads its own latched ADC-driver/capacity failure state. */
bool InputSnapshot::failed() const
{
    return _broken;
}

/** @brief Export the matching run after stop, preserving actual sample times.
 * The SD smoke build requires all 300 scheduled observations without missed
 * slots. For the RAM experiment, at least 200 measurements spanning 250 ms are required for clean collection;
 * P4 may request stop just before this window ends. This flag does not assess
 * waveform shape. The PC also validates matching run IDs, row counts and order.
 */
void InputSnapshot::report(unsigned long run, bool hardwareClean)
{
    _collecting = false;

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
#if ADS127_HARDWARE_SMOKE
    const bool clean = hardwareClean && !_broken && _missed == 0 && _count == 300 &&
        _points[299]._timeUs >= 29950000;
#else
    const bool clean = hardwareClean && !_broken && _count >= 200 &&
        _points[_count - 1]._timeUs - _points[0]._timeUs >= 250000;
#endif
    Serial.printf("INPUT_BEGIN run=%lu count=%u clean=%u missed=%lu maxGapUs=%lu mode=burst\n",
                  run, _count, unsigned(clean), static_cast<unsigned long>(_missed),
                  static_cast<unsigned long>(_maxGapUs));
    for (unsigned index = 0; index < _count; ++index)
    {
        const auto& point = _points[index];
        Serial.printf("INPUT,%u,%lu,%.6f,%d,%d,%lu,%lu\n", index,
                      static_cast<unsigned long>(point._timeUs), double(point._sum) / point._reads,
                      point._minimum, point._maximum, static_cast<unsigned long>(point._reads),
                      static_cast<unsigned long>(point._durationUs));
    }
    Serial.printf("INPUT_END run=%lu\n", run);
}
#endif
