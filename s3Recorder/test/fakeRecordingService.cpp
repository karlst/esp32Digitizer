/**
 * @file fakeRecordingService.cpp
 * @brief Replace the storage task for acquisition state-machine tests only.
 * The separate writer tests exercise real buffering; these methods let command
 * tests choose file-open failure without needing an SD card or FreeRTOS scheduler.
 * These definitions deliberately replace the production service at link time.
 * testStorageOpen/testStorageFinish inject failure results; testStorageComplete
 * records the requested final file disposition; testStorageDeletes counts calls.
 * The fake does not launch a thread, mount a filesystem, or prove card behavior.
 * Its prepare() still resets the real ring, letting acquisition tests inspect
 * whether every accepted interrupt sample was actually submitted.
 */
#include "recordingService.h"
#include <Arduino.h>
bool testStorageOpen = true;
bool testStorageFinish = true;
bool testStorageComplete = false;
uint32_t testStorageDeletes = 0;
/**
 * @brief Return simulated microseconds for writer statistics.
 */
uint64_t recordingService::clockUs() { return testUs; }
/**
 * @brief Pretend a storage worker exists.
 */
bool recordingService::begin() { return true; }
/**
 * @brief Reset the real ring on a successful simulated file opening.
 */
bool recordingService::prepare(uint32_t)
{
    if (testStorageOpen) { writer.start(sink, clockUs); }
    return testStorageOpen;
}
/**
 * @brief Remember whether acquisition requested a complete or faulted file.
 */
bool recordingService::finish(bool complete)
{
    testStorageComplete = complete;
    return testStorageFinish;
}
/**
 * @brief Count deletion requests; no filesystem is touched.
 */
bool recordingService::erase() { ++testStorageDeletes; return true; }
/**
 * @brief Provide a stable empty recording status for framing tests.
 */
recordingStatus recordingService::snapshot() { return {}; }
/**
 * @brief Expose the real byte queue so acquisition tests can count submissions.
 */
bufferedWriter& recordingService::buffer() { return writer; }
/**
 * @brief Use the actual overflow flag in acquisition tests.
 */
bool recordingService::failed() const { return writer.failed(); }
/**
 * @brief Accept fake writes without hardware.
 */
size_t sdRecordingSink::write(const uint8_t*, size_t length) { return length; }
/**
 * @brief Simulate successful filesystem synchronization.
 */
bool sdRecordingSink::flush() { return true; }
