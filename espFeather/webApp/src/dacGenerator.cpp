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
 * @file DacGenerator.cpp
 * @brief Task-timed DAC generation and timestamped ADC loopback snapshots.
 *
 * Browser requests run in Arduino's loop task; waveform updates run in a separate
 * FreeRTOS worker. An ESP timer wakes that worker at nominal 500 us intervals.
 * HTTP latency therefore does not directly determine the waveform sample rate.
 * DAC means digital-to-analog converter: an 8-bit number sets the A1 output.
 * ADC means analog-to-digital converter: Feather's own A2 input measures that
 * voltage through the loopback wire. This is separate from the external ADC.
 * This generator belongs to the web application, not the P4 bench firmware's
 * benchDac implementation. Do not apply its calibration or settings to that test.
 * Read begin() for task/timer setup, start() for settings, sample() for analog
 * work, and getStateJson() for the browser snapshot. A task is a scheduled thread;
 * a notification wakes it without carrying a sample value.
 * Shared data is locked only while reading/changing it; JSON formatting and network
 * transmission happen outside the lock so they cannot hold up sampling for long.
 */
#include "DacGenerator.h"
#include <driver/adc.h>
#include <driver/dac.h>
#include <cmath>
#include <algorithm>
#include <vector>

/**
 * @brief Prepare analog hardware, create the sampling worker, then start its clock.
 * @return True when both hardware initialization and periodic timer startup succeed.
 *
 * Called once from webApp::begin(), before requests can access this object. Startup
 * keeps DAC code zero. Resources are created in dependency order: mutex, worker,
 * timer. The timer must not fire until its notification recipient exists.
 */
bool DacGenerator::begin()
{
    bool retVal = false;
    // Create the shared-state lock before any task can access waveform settings or
    // history. Mutex means mutual exclusion: only one task may hold this lock.
    // A mutex is a FreeRTOS object; this variable stores its handle, not a
    // Boolean flag. A null handle indicates allocation failure. Taking it blocks
    // only the calling task until the owner releases it; it does not stop the CPU.
    _mutex = xSemaphoreCreateMutex();
    // A1/GPIO25 is DAC channel 1; its 8-bit code zero is our disabled state.
    // A2/GPIO34 is ADC1 channel 6. ADC1 remains usable while Wi-Fi is active.
    // Select 12-bit readings (0..4095) and input attenuation for the chosen voltage
    // range. Short-circuit evaluation stops configuration at the first driver error.
    const bool analogReady = dac_output_enable(DAC_CHANNEL_1) == ESP_OK &&
        dac_output_voltage(DAC_CHANNEL_1, 0) == ESP_OK &&
        adc1_config_width(ADC_WIDTH_BIT_12) == ESP_OK &&
        adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_12) == ESP_OK;
    if (_mutex && analogReady)
    {
        // Build the raw-count-to-millivolt conversion using factory eFuse data when
        // available. eFuse is calibration information programmed into the chip.
        // 1100 mV is the fallback ADC reference, not the board supply.
        // Save the source so the UI reveals whether factory calibration was available.
        _calibrationSource = esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12,
            ADC_WIDTH_BIT_12, 1100, &_calibration);
        _phaseStartUs = esp_timer_get_time();
        // Create a sleeping worker: 4096-byte stack, this as its context, priority 3,
        // and core 1. The handle is needed by the timer to notify it. Higher priority
        // than the usual Arduino loop helps latency but cannot guarantee exact timing.
        if (xTaskCreatePinnedToCore(taskEntry, "dacSampler", 4096, this, 3, &_worker, 1) == pdPASS)
        {
            // ESP_TIMER_TASK runs callbacks in ESP's shared timer-service task, not
            // an interrupt. Keep it short so other timers are not delayed. Passing
            // this lets the static callback recover the correct generator instance.
            esp_timer_create_args_t args = {};
            args.callback = timerCallback;
            args.arg = this;
            args.dispatch_method = ESP_TIMER_TASK;
            args.name = "dacTick";
            // Do not request replay of every elapsed timer event after a delay.
            // The worker also collapses accumulated notifications into one update.
            args.skip_unhandled_events = true;
            if (esp_timer_create(&args, &_timer) == ESP_OK)
            {
                retVal = esp_timer_start_periodic(_timer, _samplePeriodUs) == ESP_OK;
            }
        }
    }
    _ready = retVal;
    // Failure must leave neither an orphaned worker nor a timer notifying a deleted
    // task. Tear down in reverse dependency order; no HTTP access exists yet, and
    // a timer that failed to start cannot be generating new notifications.
    if (!retVal)
    {
        if (_timer)
        {
            esp_timer_delete(_timer);
            _timer = nullptr;
        }
        if (_worker)
        {
            vTaskDelete(_worker);
            _worker = nullptr;
        }
        if (_mutex)
        {
            vSemaphoreDelete(_mutex);
            _mutex = nullptr;
        }
        dac_output_voltage(DAC_CHANNEL_1, 0);
        Serial.println("DAC sampling initialization failed.");
    }
    return retVal;
}

/**
 * @brief Validate settings and start the DAC in one locked transaction.
 * @param frequency Requested cycles per second.
 * @param amplitude Peak excursion above/below the offset, in volts (not peak-to-peak).
 * @param offset Center voltage of the sine wave.
 * @return False without changing settings when validation or initialization fails.
 *
 * The minimum/maximum waveform voltages are offset-amplitude and offset+amplitude.
 * Checking their range keeps the entire waveform within the loopback experiment's
 * limits. This is enforced here even when a caller bypasses browser validation.
 */
bool DacGenerator::start(float frequency, float amplitude, float offset)
{
    bool retVal = false;
    // Permit only rounding error at decimal voltage boundaries, not a wider signal range.
    constexpr float voltageTolerance = 0.000001f;
    if (_ready && std::isfinite(frequency) && std::isfinite(amplitude) && std::isfinite(offset) &&
        frequency >= _minFrequencyHz && frequency <= _maxFrequencyHz && floorf(frequency) == frequency && amplitude >= 0 &&
        offset - amplitude >= _minSignalVolts - voltageTolerance &&
        offset + amplitude <= _maxSignalVolts + voltageTolerance)
    {
        // Apply all settings and reset the phase/history as one transaction. Without
        // the lock, the worker could read a new amplitude with an old offset. Waiting
        // with portMAX_DELAY suspends this task until the lock is available; every
        // successful take must be paired with a give, including future error paths.
        xSemaphoreTake(_mutex, portMAX_DELAY);
        // Retry of the same Start is harmless; a different Start cannot silently
        // retune running output. Stop must occur before changing its settings.
        if (!_enabled)
        {
            _frequencyHz = frequency;
            _amplitudeVolts = amplitude;
            _offsetVolts = offset;
            _phaseStartUs = esp_timer_get_time();
            _sampleCount = 0;
            _writeIndex = 0;
            _lastCaptureUs = _phaseStartUs;
            _enabled = true;
            retVal = true;
        }
        else
        {
            retVal = _frequencyHz == frequency && _amplitudeVolts == amplitude && _offsetVolts == offset;
        }
        xSemaphoreGive(_mutex);
    }
    return retVal;
}

/**
 * @brief Start or stop output from the web task, synchronizing with the worker.
 * @return Whether the analog service was initialized and can accept the command.
 *
 * Repeated Start or Stop requests do not reset an already matching state. Stop
 * requests code zero before returning; Start allows the next worker tick to write a sine
 * sample. Driver write errors are not checked here, so true confirms software
 * acceptance, not measured voltage. The DAC remains electrically enabled.
 */
bool DacGenerator::setEnabled(bool requested)
{
    bool retVal = _ready;
    if (_ready)
    {
        // Hold the same lock as sample() so Stop cannot race a worker that already
        // calculated a nonzero output. Once Stop returns, later samples see enabled=false.
        xSemaphoreTake(_mutex, portMAX_DELAY);
        if (_enabled != requested)
        {
            _enabled = requested;
            _phaseStartUs = esp_timer_get_time();
            _sampleCount = 0;
            _writeIndex = 0;
            _lastCaptureUs = _phaseStartUs;
            // Request zero while holding the lock, so the worker cannot write
            // a stale nonzero command afterward. This driver result is unchecked.
            if (!_enabled)
            {
                dac_output_voltage(DAC_CHANNEL_1, 0);
                _lastCode = 0;
            }
        }
        xSemaphoreGive(_mutex);
    }
    return retVal;
}

/**
 * @brief Wake the worker from ESP's timer-service task without doing analog work here.
 * @param context The generator pointer supplied as args.arg during begin().
 *
 * A static callback has no implicit this pointer. Cast the saved context back to
 * our instance, then increment the worker's notification count. This is a task
 * callback, so the normal notification API is used rather than an ISR variant.
 */
void DacGenerator::timerCallback(void* context)
{
    auto* dacGenerator = static_cast<DacGenerator*>(context);
    xTaskNotifyGive(dacGenerator->_worker);
}

/**
 * @brief Worker entry point: sleep until notified, then perform one current update.
 * @param context The generator pointer passed when the FreeRTOS task was created.
 *
 * pdTRUE clears all pending notifications on wake. If several ticks arrived while
 * the worker was delayed, it samples once at the current time instead of emitting
 * a burst of stale DAC values. portMAX_DELAY lets it sleep without busy-waiting.
 */
void DacGenerator::taskEntry(void* context)
{
    auto* dacGenerator = static_cast<DacGenerator*>(context);
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        dacGenerator->sample();
    }
}

/**
 * @brief Acquire one ADC reading and write the next DAC sample from the worker task.
 *
 * Order matters: measure A2 while the previous A1 code is still held, record that
 * code with the measurement, then advance A1. Pairing the reading with the next code
 * would give the graph an artificial one-update mismatch. nowUs approximates the
 * start of ADC acquisition; conversion and scheduling are not instantaneous.
 */
void DacGenerator::sample()
{
    // Keep settings, phase, history, and the physical DAC write in one protected
    // transaction. start(), setEnabled(), and getStateJson() share this lock.
    // ADC work lengthens the hold time; web code must therefore avoid slow work
    // while holding the lock. This design favors simple, coherent sample records.
    xSemaphoreTake(_mutex, portMAX_DELAY);
    const int64_t nowUs = esp_timer_get_time();
    // Estimate missed update intervals from elapsed time, rather than claiming a
    // perfectly regular sample clock. This counts whole extra 500 us intervals in
    // a gap; it is a diagnostic, not a precise count of lost hardware timer edges.
    if (_lastSampleUs && nowUs - _lastSampleUs >= 2 * _samplePeriodUs)
    {
        _missedIntervals += (nowUs - _lastSampleUs) / _samplePeriodUs - 1;
    }
    _lastSampleUs = nowUs;
    // Convert the ADC's raw 12-bit count with the coefficients chosen at startup.
    // lastCode still identifies the DAC value present during this acquisition.
    const int raw = adc1_get_raw(ADC1_CHANNEL_6);
    const uint32_t millivolts = esp_adc_cal_raw_to_voltage(raw, &_calibration);

    // The graph ring has only 256 slots. At 1 Hz, saving every 500 us would retain
    // only 0.128 s, far short of a full period. Save only selected records to
    // retain roughly two periods at low frequencies, while still updating the DAC
    // and reading the ADC on every tick. This selective saving is called decimation;
    // it affects graph history, not waveform timing. At high frequencies save each sample.
    const int64_t capturePeriodUs = std::max<int64_t>(_samplePeriodUs,
        static_cast<int64_t>(2000000.0f / (_frequencyHz * (_historySize - 1))));
    if (nowUs - _lastCaptureUs >= capturePeriodUs)
    {
        _sampleRecords[_writeIndex] = {static_cast<uint32_t>(nowUs),
            static_cast<uint16_t>(millivolts), _lastCode};
        _writeIndex = (_writeIndex + 1) % _historySize;
        _sampleCount = std::min(_sampleCount + 1, _historySize);
        _lastCaptureUs = nowUs;
    }

    // Compute phase from elapsed real time rather than incrementing a phase counter:
    // a delayed task skips ahead instead of slowing the requested frequency down.
    // fmod keeps the sine argument within one cycle. Convert volts to the nearest
    // 8-bit DAC code using a nominal 3.3 V supply; this is not DAC calibration, so
    // the measured trace may differ. Disabled output always writes code zero.
    uint8_t nextCode = 0;
    if (_enabled)
    {
        const double cycles = (nowUs - _phaseStartUs) * _frequencyHz / 1000000.0;
        const float volts = _offsetVolts + _amplitudeVolts * sin(2.0 * PI * fmod(cycles, 1.0));
        nextCode = static_cast<uint8_t>(lroundf(volts * 255.0f / _nominalSupplyVolts));
    }
    // This is the actual DAC command. Its return status is not checked;
    // lastCode records the request, not a verified physical output voltage.
    dac_output_voltage(DAC_CHANNEL_1, nextCode);
    _lastCode = nextCode;
    xSemaphoreGive(_mutex);
}

/**
 * @brief Return settings and graph samples as JSON for the web task's HTTP response.
 * @return A coherent snapshot, or {"ready":false} after initialization failure.
 *
 * First allocate temporary storage, then lock only long enough to copy shared
 * state. Release the lock before formatting numbers and allocating the JSON
 * string. Network transmission happens later in webController with no lock held.
 */
String DacGenerator::getStateJson()
{
    String retVal = "{\"ready\":false}";
    if (_ready)
    {
        std::vector<SampleRecord> samples(_historySize);
        // Snapshot settings and ring indices together so a settings change cannot
        // attach new parameters to an unrelated history window during this copy.
        xSemaphoreTake(_mutex, portMAX_DELAY);
        const float frequency = _frequencyHz;
        const float amplitude = _amplitudeVolts;
        const float offset = _offsetVolts;
        const bool active = _enabled;
        const uint32_t missed = _missedIntervals;
        const size_t count = _sampleCount;
        // The oldest record is count positions behind the next write slot. Modular
        // indexing unwraps the circular buffer into chronological order for the browser.
        for (size_t i = 0; i < count; ++i)
        {
            samples[i] = _sampleRecords[(_writeIndex + _historySize - count + i) % _historySize];
        }
        xSemaphoreGive(_mutex);

        // Samples are [elapsed milliseconds, commanded nominal volts, measured volts].
        // Unsigned subtraction handles the low-32-bit microsecond timer wrapping
        // (~71.6 minutes), since the retained window is only a few seconds long.
        // Reserve space once to reduce reallocations as the response is assembled.
        retVal.reserve(12000);
        retVal = "{\"ready\":true,\"enabled\":" + String(active ? "true" : "false");
        retVal += ",\"frequencyHz\":" + String(frequency, 3);
        retVal += ",\"amplitudeVolts\":" + String(amplitude, 3);
        retVal += ",\"offsetVolts\":" + String(offset, 3);
        retVal += ",\"minFrequencyHz\":" + String(_minFrequencyHz);
        retVal += ",\"maxFrequencyHz\":" + String(_maxFrequencyHz);
        retVal += ",\"minSignalVolts\":" + String(_minSignalVolts);
        retVal += ",\"maxSignalVolts\":" + String(_maxSignalVolts);
        retVal += ",\"sampleRateHz\":" + String(1000000 / _samplePeriodUs);
        retVal += ",\"missedIntervals\":" + String(missed);
        retVal += ",\"calibration\":\"" + String(_calibrationSource == ESP_ADC_CAL_VAL_DEFAULT_VREF ?
            "Nominal reference" : "Factory eFuse") + "\",\"samples\":[";
        for (size_t i = 0; i < count; ++i)
        {
            if (i)
            {
                retVal += ',';
            }
            retVal += '[' + String(static_cast<uint32_t>(samples[i]._timeUs - samples[0]._timeUs) / 1000.0f, 3);
            retVal += ',' + String(samples[i]._command * _nominalSupplyVolts / 255.0f, 3);
            retVal += ',' + String(samples[i]._millivolts / 1000.0f, 3) + ']';
        }
        retVal += "]}";
    }
    return retVal;
}
