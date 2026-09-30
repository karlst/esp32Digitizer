/**
 * @file dacGenerator.h
 * @brief Timed sine output on A1 and ADC1 loopback measurement on A2.
 */
#pragma once
#include <Arduino.h>
#include <esp_timer.h>
#include <esp_adc_cal.h>
#include <freertos/semphr.h>

/**
 * @brief Generate a sine wave independently of browser traffic and measure its loopback.
 *
 * Three execution contexts cooperate:
 * - Arduino's loop task handles HTTP and calls start(), setEnabled(), stateJson().
 * - ESP's timer service calls timerCallback() every nominal 500 microseconds.
 * - Our worker task wakes on that notification and calls sample().
 *
 * The timer callback only wakes the worker; it never takes the mutex. The web
 * task and worker share settings, phase origin, DAC state, and the history ring.
 * They must hold mutex while accessing those mutable values. Pinning the worker
 * to the same core as loop() does not eliminate task switching or this requirement.
 * Initialization precedes HTTP service; ready and calibration are immutable afterward.
 */
class dacGenerator
{
public:
    static constexpr float minFrequencyHz = 1.0f;
    static constexpr float maxFrequencyHz = 100.0f;
    static constexpr float nominalSupplyVolts = 3.3f;
    // Keep the initial loopback experiment inside the useful ADC measurement range.
    static constexpr float minSignalVolts = 0.2f;
    static constexpr float maxSignalVolts = 2.4f;
    // Nominal 2 kHz updates give 20 DAC steps per period at the 100 Hz maximum.
    // Task scheduling introduces jitter; this is not a hardware-clocked DAC stream.
    static constexpr uint32_t samplePeriodUs = 500;
    /** @brief Initialize DAC/ADC and start sampling with waveform output disabled. */
    bool begin();
    /** @brief Validate settings and atomically start output; running settings cannot change. */
    bool start(float frequency, float amplitude, float offset);
    /** @brief Enable or disable the waveform; disabled output is DAC code zero. */
    bool setEnabled(bool enabled);
    /** @brief Serialize a coherent snapshot of settings and measured graph samples. */
    String stateJson();

private:
    /** @brief Wake the worker without doing analog conversions in the timer callback. */
    static void timerCallback(void* context);
    /** @brief Run analog sampling independently of HTTP handling. */
    static void taskEntry(void* context);
    /** @brief Measure the previous output, then write the next time-based sine sample. */
    void sample();

    /**
     * @brief Pair an ADC reading with the DAC code held during its acquisition.
     * timeUs stores the low 32 bits of microseconds since boot; snapshots subtract
     * timestamps with unsigned arithmetic, so a wrap within the short window is safe.
     */
    struct sampleRecord
    {
        uint32_t timeUs;
        uint16_t millivolts;
        uint8_t command;
    };
    static constexpr size_t historySize = 256;
    // Bounded circular buffer: writeIndex is the next slot to replace; sampleCount
    // grows to historySize and then stays there. All three are protected by mutex.
    sampleRecord history[historySize] = {};
    size_t sampleCount = 0;
    size_t writeIndex = 0;
    float frequencyHz = 10.0f;
    float amplitudeVolts = 0.5f;
    float offsetVolts = 1.5f;
    bool enabled = false;
    // Set once during startup before HTTP handling begins, then read without a lock.
    bool ready = false;
    uint8_t lastCode = 0;
    uint32_t missedIntervals = 0;
    int64_t phaseStartUs = 0;
    int64_t lastSampleUs = 0;
    int64_t lastCaptureUs = 0;
    // FreeRTOS mutex = exclusive access to shared state, with priority inheritance.
    // It prevents mixed settings, torn history copies, and a sample writing a
    // nonzero DAC code after a completed Stop request. It does not control timing.
    SemaphoreHandle_t mutex = nullptr;
    TaskHandle_t worker = nullptr;
    esp_timer_handle_t timer = nullptr;
    // ADC conversion coefficients are established once before starting the worker.
    // They calibrate ADC readings, not the commanded DAC voltage estimate.
    esp_adc_cal_characteristics_t calibration = {};
    esp_adc_cal_value_t calibrationSource = ESP_ADC_CAL_VAL_DEFAULT_VREF;
};
