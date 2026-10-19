/**
 * @file benchDac.h
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
 * @brief Serialize periodic A1 updates with start/stop in the Arduino loop.
 * ESP_TIMER_TASK invokes tick() in a shared task, NOT an interrupt. A mutex
 * prevents stop() from disabling the DAC while a callback writes a new code.
 * No DAC output is enabled by construction or initialize(). Objects live for
 * the firmware lifetime; timer/mutex storage is reused between finite runs.
 */
class benchDac
{
public:
    bool initialize();
    bool start();
    void stop();
    bool failed();
private:
    static void tick(void* context);
    SemaphoreHandle_t mutex = nullptr;
    esp_timer_handle_t timer = nullptr;
    bool enabled = false, broken = false;
    int64_t beganAt = 0, lastAt = 0;
    uint32_t updates = 0, missed = 0;
    uint64_t maxGapUs = 0;
};
#endif
