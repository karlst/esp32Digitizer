/**
 * @file recordingService.h
 * @brief Coordinate recording commands without making the ADC task write to disk.
 */
#pragma once
#include "sdRecordingSink.h"
#include "recordingSpace.h"
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
    // Private cross-core command names. none means no outstanding operation;
    // prepare opens a new recording; finish saves/closes; erase deletes recordings.
    enum class operation : uint32_t { none, prepare, finish, erase };
    bool request(operation action, uint32_t argument);
    static void taskEntry(void* argument);
    static uint64_t clockUs();
    void run();
    void publish();
    void refreshSpace();
    // Only writer.submit() runs on the sample-producing side. All other writer
    // operations and every sink method belong to this service's disk thread.
    bufferedWriter writer;
    sdRecordingSink sink;
    recordingSpace spaceEstimate;
    bool spaceEstimateValid = false;
    // Mailbox handoff: acquisition writes argument then pending; disk thread
    // reads them, completes work, writes result, then clears pending.
    std::atomic<operation> pending{operation::none};
    std::atomic<bool> result{false};
    // Not independently atomic: the pending release/acquire handoff protects it.
    // For prepare it is samples/second; for finish 1=normal, 0=acquisition fault.
    uint32_t argument = 0;
    // available is thread-creation success, not card readiness. recording is
    // owned by the disk thread and means a recording file is active.
    bool available = false, recording = false;
    // current is the disk thread's working copy. published is the last complete
    // display copy shared under statusLock. Never share current directly.
    recordingStatus current, published;
    portMUX_TYPE statusLock = portMUX_INITIALIZER_UNLOCKED;
    // Scheduler times for display copying, free-space queries, and throughput.
    // rateBytes is the prior payload total used to compute bytes per second.
    uint32_t lastPublishMs = 0, rateMs = 0;
    uint64_t rateBytes = 0;
};
