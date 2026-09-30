/**
 * @file fastCapture.h
 * @brief Short interrupt-time SPI reads for the ADS1256's 30000-sample/second mode.
 */
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>

/**
 * @brief Read each ready sample before a task switch can use up its time window.
 *
 * At 30k, samples are only 33.3 microseconds apart. The ordinary path wakes a
 * task before reading; measured wake delay can consume half that interval.
 * This class performs just the fixed three-byte read inside the GPIO interrupt.
 * Setup, calibration, commands, printing and publication remain in tasks.
 *
 * The acquisition task configures the ADC/SPI first, resets this object, then
 * attaches its interrupt on core 0. While attached, only onReady() may touch SPI.
 * Detach the interrupt before a task sends any other SPI command. snapshot()
 * must run on the same core and briefly masks interrupts while copying results.
 * No sample history is retained: this preserves the existing latest/count design.
 */
class fastCapture
{
public:
    /**
     * @brief Latest sample and run totals written only by the interrupt handler.
     * fault uses the existing flags: 1 read failure, 2 overlapping ready event.
     * A fault freezes this record until reset; it never silently skips a bad read.
     */
    struct captureSnapshot
    {
        uint64_t count = 0;
        uint32_t events = 0;
        int32_t raw = 0;
        uint32_t sampleUs = 0;
        uint32_t fault = 0;
        uint32_t stage = 0;
        uint32_t readUs = 0;
        uint32_t maxReadUs = 0;
        uint32_t maxGapUs = 0;
        uint32_t previousEventUs = 0;
    };
    void reset();
    void IRAM_ATTR onReady();
    captureSnapshot snapshot();
private:
    // This lock masks the core-0 interrupt during the task's short copy. ISR never
    // takes a task mutex, allocates memory, or calls the Arduino SPI library.
    portMUX_TYPE copyLock = portMUX_INITIALIZER_UNLOCKED;
    captureSnapshot state;
};
