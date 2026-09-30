/**
 * @file recordingService.h
 * @brief Coordinate recording commands without making the ADC task write to disk.
 */
#pragma once
#include "sdRecordingSink.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/**
 * @brief Own the storage task, adapter and reusable writer for one recorder.
 * Acquisition requests prepare/finish/delete, then waits while communications
 * continues to report progress. During capture only the byte writer is touched
 * by the producer. This class translates its generic statistics for Feather.
 */
class recordingService
{
public:
    bool begin();
    bool prepare(uint32_t rate);
    bool finish(bool complete);
    bool erase();
    recordingStatus snapshot();
    bufferedWriter& buffer();
    bool failed() const;
private:
    enum class operation : uint32_t { none, prepare, finish, erase };
    bool request(operation action, uint32_t argument);
    static void taskEntry(void* argument);
    static uint64_t clockUs();
    void run();
    void publish();
    bufferedWriter writer;
    sdRecordingSink sink;
    std::atomic<operation> pending{operation::none};
    std::atomic<bool> result{false};
    uint32_t argument = 0;
    bool available = false, recording = false;
    recordingStatus current, published;
    portMUX_TYPE statusLock = portMUX_INITIALIZER_UNLOCKED;
    uint32_t lastPublishMs = 0, lastSpaceMs = 0, rateMs = 0;
    uint64_t rateBytes = 0;
};
