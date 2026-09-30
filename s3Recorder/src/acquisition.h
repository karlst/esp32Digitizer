/**
 * @file acquisition.h
 * @brief Isolate time-sensitive sample collection from UART formatting and commands.
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
 * @brief Sole owner of ADC/SPI; loop() submits commands and reads copied snapshots.
 *
 * A FreeRTOS queue transfers commands safely between cores. A short critical
 * section protects snapshot copies, including the 64-bit count (not atomic on
 * a 32-bit CPU). No SPI, waits, text formatting, or UART writes occur under that
 * lock. Thus a slow status reader cannot hold up the digitizer for milliseconds.
 */
class acquisition
{
public:
    bool begin();
    bool submit(const acquisitionCommand& command);
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
    acquisitionStatus current;
    acquisitionStatus published;
    // One ISR writer and one same-core task reader; aligned 32-bit loads/stores
    // are indivisible on ESP32-S3. Volatile forces re-reading around SPI activity.
    volatile uint32_t readyEdges = 0;
    uint32_t previousEdge = 0;
    uint32_t lastReadMs = 0;
    uint32_t rateWindowMs = 0;
    uint64_t rateWindowCount = 0;
    uint32_t lastPublishMs = 0;
    commandHistory history;
};
