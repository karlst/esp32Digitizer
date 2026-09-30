/**
 * @file acquisition.h
 * @brief Declare the S3 background task and the state it shares with communications.
 * Read acquisition.cpp for the startup-to-sample sequence and FreeRTOS call details.
 */
#pragma once
#include "ads1256.h"
#include "commandProtocol.h"
#include "commandHistory.h"
#include "acquisitionStatus.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

/**
 * @brief Give one background task exclusive responsibility for the digitizer.
 * ADC means analog-to-digital converter; SPI is the serial bus used to read it.
 * Arduino loop() submits requests instead of touching SPI directly. The task
 * handles those requests between read attempts, so commands and reads cannot
 * fight over the device.
 *
 * A FreeRTOS queue transfers commands safely between cores. A short critical
 * section protects snapshot copies, including the 64-bit count (not atomic on
 * a 32-bit CPU). No SPI, waits, text formatting, or UART writes occur under that
 * lock. Thus a slow status reader cannot hold up the digitizer for milliseconds.
 */
class acquisition
{
public:
    /**
     * @brief Create queue/task only; hardware readiness arrives later in status.
     */
    bool begin();
    /**
     * @brief Copy one request to the pending queue; false means not queued.
     */
    bool submit(const acquisitionCommand& command);
    /**
     * @brief Copy the last published status under a brief cross-core lock.
     */
    acquisitionStatus snapshot();
private:
    // Desktop tests step this otherwise permanent worker with fake hardware.
    friend class nativeChecks;
    static void taskEntry(void* argument);
    static void IRAM_ATTR readyInterrupt(void* argument);
    void run();
    void collect();
    void execute(const acquisitionCommand& command);
    void publish();
    void fault(uint32_t error);
    ads1256 adc;
    TaskHandle_t worker = nullptr;
    QueueHandle_t commands = nullptr;
    portMUX_TYPE snapshotLock = portMUX_INITIALIZER_UNLOCKED;
    // current belongs to this task; published is the copy shared with the other
    // core. Protect that copy with snapshotLock, not the entire sampling process.
    acquisitionStatus current;
    acquisitionStatus published;
    // One ISR writer and one same-core task reader; aligned 32-bit loads/stores
    // are indivisible on ESP32-S3. Volatile forces re-reading around SPI activity.
    volatile uint32_t readyEdges = 0;
    // Previous accepted read's event count; lastReadMs drives the no-data timeout.
    uint32_t previousEdge = 0;
    uint32_t lastReadMs = 0;
    // The rate calculation compares successful count/time against these saved
    // values roughly once a second. The cumulative sample count does not reset.
    uint32_t rateWindowMs = 0;
    uint64_t rateWindowCount = 0;
    uint32_t lastPublishMs = 0;
    commandHistory history;
};
