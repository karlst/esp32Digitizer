/**
 * @file dacGenerator.cpp
 * @brief Task-timed DAC generation and timestamped ADC loopback snapshots.
 *
 * Browser requests run in Arduino's loop task; waveform updates run in a separate
 * FreeRTOS worker. An ESP timer wakes that worker at nominal 500 us intervals.
 * HTTP latency therefore does not directly determine the waveform sample rate.
 * Shared data is locked only while reading/changing it; JSON formatting and network
 * transmission happen outside the lock so they cannot hold up sampling for long.
 */
#include "dacGenerator.h"
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
bool dacGenerator::begin()
{
    bool retVal = false;
    // Create the shared-state lock before any task can access waveform settings or
    // history. A mutex is a FreeRTOS object; this variable stores its handle, not a
    // Boolean flag. A null handle indicates allocation failure. Taking it blocks
    // only the calling task until the owner releases it; it does not stop the CPU.
    mutex = xSemaphoreCreateMutex();
    // A1/GPIO25 is DAC channel 1; its 8-bit code zero is our disabled state.
    // A2/GPIO34 is ADC1 channel 6. ADC1 remains usable while Wi-Fi is active.
    // Select 12-bit readings (0..4095) and input attenuation for the chosen voltage
    // range. Short-circuit evaluation stops configuration at the first driver error.
    const bool analogReady = dac_output_enable(DAC_CHANNEL_1) == ESP_OK &&
        dac_output_voltage(DAC_CHANNEL_1, 0) == ESP_OK &&
        adc1_config_width(ADC_WIDTH_BIT_12) == ESP_OK &&
        adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_12) == ESP_OK;
    if (mutex && analogReady)
    {
        // Build the raw-count-to-millivolt conversion using factory eFuse data when
        // available. 1100 mV is the fallback ADC reference, not the board supply.
        // Save the source so the UI reveals whether factory calibration was available.
        calibrationSource = esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12,
            ADC_WIDTH_BIT_12, 1100, &calibration);
        phaseStartUs = esp_timer_get_time();
        // Create a sleeping worker: 4096-byte stack, this as its context, priority 3,
        // and core 1. The handle is needed by the timer to notify it. Higher priority
        // than the usual Arduino loop helps latency but cannot guarantee exact timing.
        if (xTaskCreatePinnedToCore(taskEntry, "dacSampler", 4096, this, 3, &worker, 1) == pdPASS)
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
            if (esp_timer_create(&args, &timer) == ESP_OK)
            {
                retVal = esp_timer_start_periodic(timer, samplePeriodUs) == ESP_OK;
            }
        }
    }
    ready = retVal;
    // Failure must leave neither an orphaned worker nor a timer notifying a deleted
    // task. Tear down in reverse dependency order; no HTTP access exists yet, and
    // a timer that failed to start cannot be generating new notifications.
    if (!retVal)
    {
        if (timer)
        {
            esp_timer_delete(timer);
            timer = nullptr;
        }
        if (worker)
        {
            vTaskDelete(worker);
            worker = nullptr;
        }
        if (mutex)
        {
            vSemaphoreDelete(mutex);
            mutex = nullptr;
        }
        dac_output_voltage(DAC_CHANNEL_1, 0);
        Serial.println("DAC sampling initialization failed.");
    }
    return retVal;
}

/**
 * @brief Validate and apply a complete set of sine parameters from the web task.
 * @param frequency Requested cycles per second.
 * @param amplitude Peak excursion above/below the offset, in volts (not peak-to-peak).
 * @param offset Center voltage of the sine wave.
 * @return False without changing settings when validation or initialization fails.
 *
 * The minimum/maximum waveform voltages are offset-amplitude and offset+amplitude.
 * Checking their range keeps the entire waveform within the loopback experiment's
 * limits. This is enforced here even when a caller bypasses browser validation.
 */
bool dacGenerator::configure(float frequency, float amplitude, float offset)
{
    bool retVal = false;
    // Permit only rounding error at decimal voltage boundaries, not a wider signal range.
    constexpr float voltageTolerance = 0.000001f;
    if (ready && std::isfinite(frequency) && std::isfinite(amplitude) && std::isfinite(offset) &&
        frequency >= minFrequencyHz && frequency <= maxFrequencyHz && amplitude >= 0 &&
        offset - amplitude >= minSignalVolts - voltageTolerance &&
        offset + amplitude <= maxSignalVolts + voltageTolerance)
    {
        // Apply all settings and reset the phase/history as one transaction. Without
        // the lock, the worker could read a new amplitude with an old offset. Waiting
        // with portMAX_DELAY suspends this task until the lock is available; every
        // successful take must be paired with a give, including future error paths.
        xSemaphoreTake(mutex, portMAX_DELAY);
        frequencyHz = frequency;
        amplitudeVolts = amplitude;
        offsetVolts = offset;
        phaseStartUs = esp_timer_get_time();
        sampleCount = 0;
        writeIndex = 0;
        lastCaptureUs = phaseStartUs;
        xSemaphoreGive(mutex);
        retVal = true;
    }
    return retVal;
}

/**
 * @brief Start or stop output from the web task, synchronizing with the worker.
 * @return Whether the analog service was initialized and can accept the command.
 *
 * Repeated Start or Stop requests do not reset an already matching state. Stop
 * writes zero before returning; Start allows the next worker tick to write a sine
 * sample. The DAC remains enabled electrically while output is stopped (code zero).
 */
bool dacGenerator::setEnabled(bool requested)
{
    bool retVal = ready;
    if (ready)
    {
        // Hold the same lock as sample() so Stop cannot race a worker that already
        // calculated a nonzero output. Once Stop returns, later samples see enabled=false.
        xSemaphoreTake(mutex, portMAX_DELAY);
        if (enabled != requested)
        {
            enabled = requested;
            phaseStartUs = esp_timer_get_time();
            sampleCount = 0;
            writeIndex = 0;
            lastCaptureUs = phaseStartUs;
            // Disable synchronously so a completed Stop command leaves code zero.
            if (!enabled)
            {
                dac_output_voltage(DAC_CHANNEL_1, 0);
                lastCode = 0;
            }
        }
        xSemaphoreGive(mutex);
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
void dacGenerator::timerCallback(void* context)
{
    auto* generator = static_cast<dacGenerator*>(context);
    xTaskNotifyGive(generator->worker);
}

/**
 * @brief Worker entry point: sleep until notified, then perform one current update.
 * @param context The generator pointer passed when the FreeRTOS task was created.
 *
 * pdTRUE clears all pending notifications on wake. If several ticks arrived while
 * the worker was delayed, it samples once at the current time instead of emitting
 * a burst of stale DAC values. portMAX_DELAY lets it sleep without busy-waiting.
 */
void dacGenerator::taskEntry(void* context)
{
    auto* generator = static_cast<dacGenerator*>(context);
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        generator->sample();
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
void dacGenerator::sample()
{
    // Keep settings, phase, history, and the physical DAC write in one protected
    // transaction. configure(), setEnabled(), and stateJson() share this lock.
    // ADC work lengthens the hold time; web code must therefore avoid slow work
    // while holding the lock. This design favors simple, coherent sample records.
    xSemaphoreTake(mutex, portMAX_DELAY);
    const int64_t nowUs = esp_timer_get_time();
    // Estimate missed update intervals from elapsed time, rather than claiming a
    // perfectly regular sample clock. This counts whole extra 500 us intervals in
    // a gap; it is a diagnostic, not a precise count of lost hardware timer edges.
    if (lastSampleUs && nowUs - lastSampleUs >= 2 * samplePeriodUs)
    {
        missedIntervals += (nowUs - lastSampleUs) / samplePeriodUs - 1;
    }
    lastSampleUs = nowUs;
    // Convert the ADC's raw 12-bit count with the coefficients chosen at startup.
    // lastCode still identifies the DAC value present during this acquisition.
    const int raw = adc1_get_raw(ADC1_CHANNEL_6);
    const uint32_t millivolts = esp_adc_cal_raw_to_voltage(raw, &calibration);

    // The graph ring has only 256 slots. At 1 Hz, saving every 500 us would retain
    // only 0.128 s, far short of a full period. Decimate stored records enough to
    // retain roughly two periods at low frequencies, while still updating the DAC
    // and reading the ADC on every tick. At high frequencies store every sample.
    const int64_t capturePeriodUs = std::max<int64_t>(samplePeriodUs,
        static_cast<int64_t>(2000000.0f / (frequencyHz * (historySize - 1))));
    if (nowUs - lastCaptureUs >= capturePeriodUs)
    {
        history[writeIndex] = {static_cast<uint32_t>(nowUs),
            static_cast<uint16_t>(millivolts), lastCode};
        writeIndex = (writeIndex + 1) % historySize;
        sampleCount = std::min(sampleCount + 1, historySize);
        lastCaptureUs = nowUs;
    }

    // Compute phase from elapsed real time rather than incrementing a phase counter:
    // a delayed task skips ahead instead of slowing the requested frequency down.
    // fmod keeps the sine argument within one cycle. Convert volts to the nearest
    // 8-bit DAC code using a nominal 3.3 V supply; this is not DAC calibration, so
    // the measured trace may differ. Disabled output always writes code zero.
    uint8_t nextCode = 0;
    if (enabled)
    {
        const double cycles = (nowUs - phaseStartUs) * frequencyHz / 1000000.0;
        const float volts = offsetVolts + amplitudeVolts * sin(2.0 * PI * fmod(cycles, 1.0));
        nextCode = static_cast<uint8_t>(lroundf(volts * 255.0f / nominalSupplyVolts));
    }
    dac_output_voltage(DAC_CHANNEL_1, nextCode);
    lastCode = nextCode;
    xSemaphoreGive(mutex);
}

/**
 * @brief Return settings and graph samples as JSON for the web task's HTTP response.
 * @return A coherent snapshot, or {"ready":false} after initialization failure.
 *
 * First allocate temporary storage, then lock only long enough to copy shared
 * state. Release the lock before formatting numbers and allocating the JSON
 * string. Network transmission happens later in webController with no lock held.
 */
String dacGenerator::stateJson()
{
    String retVal = "{\"ready\":false}";
    if (ready)
    {
        std::vector<sampleRecord> samples(historySize);
        // Snapshot settings and ring indices together so a settings change cannot
        // attach new parameters to an unrelated history window during this copy.
        xSemaphoreTake(mutex, portMAX_DELAY);
        const float frequency = frequencyHz;
        const float amplitude = amplitudeVolts;
        const float offset = offsetVolts;
        const bool active = enabled;
        const uint32_t missed = missedIntervals;
        const size_t count = sampleCount;
        // The oldest record is count positions behind the next write slot. Modular
        // indexing unwraps the circular buffer into chronological order for the browser.
        for (size_t i = 0; i < count; ++i)
        {
            samples[i] = history[(writeIndex + historySize - count + i) % historySize];
        }
        xSemaphoreGive(mutex);

        // Samples are [elapsed milliseconds, commanded nominal volts, measured volts].
        // Unsigned subtraction handles the low-32-bit microsecond timer wrapping
        // (~71.6 minutes), since the retained window is only a few seconds long.
        // Reserve space once to reduce reallocations as the response is assembled.
        retVal.reserve(12000);
        retVal = "{\"ready\":true,\"enabled\":" + String(active ? "true" : "false");
        retVal += ",\"frequencyHz\":" + String(frequency, 3);
        retVal += ",\"amplitudeVolts\":" + String(amplitude, 3);
        retVal += ",\"offsetVolts\":" + String(offset, 3);
        retVal += ",\"minFrequencyHz\":" + String(minFrequencyHz);
        retVal += ",\"maxFrequencyHz\":" + String(maxFrequencyHz);
        retVal += ",\"minSignalVolts\":" + String(minSignalVolts);
        retVal += ",\"maxSignalVolts\":" + String(maxSignalVolts);
        retVal += ",\"sampleRateHz\":" + String(1000000 / samplePeriodUs);
        retVal += ",\"missedIntervals\":" + String(missed);
        retVal += ",\"calibration\":\"" + String(calibrationSource == ESP_ADC_CAL_VAL_DEFAULT_VREF ?
            "Nominal reference" : "Factory eFuse") + "\",\"samples\":[";
        for (size_t i = 0; i < count; ++i)
        {
            if (i)
            {
                retVal += ',';
            }
            retVal += '[' + String(static_cast<uint32_t>(samples[i].timeUs - samples[0].timeUs) / 1000.0f, 3);
            retVal += ',' + String(samples[i].command * nominalSupplyVolts / 255.0f, 3);
            retVal += ',' + String(samples[i].millivolts / 1000.0f, 3) + ']';
        }
        retVal += "]}";
    }
    return retVal;
}
