/**
 * @file recordingService.cpp
 * @brief Perform slow filesystem work on core 1, while samples arrive on core 0.
 * Read request() for command handoff, run() for disk operations, and publish()
 * for the statistics sent to Feather. The communications loop shares core 1 but
 * is a separate task: an SD wait does not hold an acquisition/status mutex.
 */
#include "recordingService.h"
#include <Arduino.h>
#include <esp_timer.h>

/** @brief Supply a 64-bit microsecond clock, avoiding millis() rollover in long runs. */
uint64_t recordingService::clockUs()
{
    const uint64_t retVal = static_cast<uint64_t>(esp_timer_get_time());
    return retVal;
}

/** @brief Launch the storage worker; mounting a card happens asynchronously there. */
bool recordingService::begin()
{
    TaskHandle_t worker = nullptr;
    available = xTaskCreatePinnedToCore(taskEntry, "diskWriter", 6144, this, 1, &worker, 1) == pdPASS;
    if (!available)
    {
        current.values[recordingStatus::state] = recordingStatus::failed;
        current.values[recordingStatus::writeErrors] = 1;
        published = current; // No worker exists, so no concurrent writer yet.
    }
    const bool retVal = available;
    return retVal;
}

/**
 * @brief Hand one operation to storage and wait by sleeping, not by busy polling.
 * Only acquisition calls this, with sampling stopped. The release/acquire atomic
 * handoff makes argument/result visible across cores without a mutex around I/O.
 * No arbitrary timeout abandons a live file: progress status keeps Feather's
 * command pending until the underlying operation actually completes or fails.
 */
bool recordingService::request(operation action, uint32_t value)
{
    bool retVal = available && pending.load(std::memory_order_acquire) == operation::none;
    if (retVal)
    {
        argument = value;
        pending.store(action, std::memory_order_release);
        while (pending.load(std::memory_order_acquire) != operation::none)
        {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        retVal = result.load(std::memory_order_acquire);
    }
    return retVal;
}

/** @brief Open a new file before acquisition starts; false leaves sampling stopped. */
bool recordingService::prepare(uint32_t rate)
{
    const bool retVal = request(operation::prepare, rate);
    return retVal;
}

/** @brief Drain and close after the producer stops; complete=false marks a faulted run. */
bool recordingService::finish(bool complete)
{
    const bool retVal = request(operation::finish, complete ? 1 : 0);
    return retVal;
}

/** @brief Execute the explicitly requested recording-file deletion while stopped. */
bool recordingService::erase()
{
    const bool retVal = request(operation::erase, 0);
    return retVal;
}

/** @brief Return the producer interface; acquisition attaches it only during recording. */
bufferedWriter& recordingService::buffer()
{
    return writer;
}

/** @brief Check the writer's atomic fault flag without reading its live statistics. */
bool recordingService::failed() const
{
    const bool retVal = writer.failed();
    return retVal;
}

/** @brief Copy a previously published storage snapshot under a short memory-only lock. */
recordingStatus recordingService::snapshot()
{
    portENTER_CRITICAL(&statusLock);
    const recordingStatus retVal = published;
    portEXIT_CRITICAL(&statusLock);
    return retVal;
}

/** @brief Recover the service object for FreeRTOS's ordinary-function entry point. */
void recordingService::taskEntry(void* argument)
{
    static_cast<recordingService*>(argument)->run();
}

/**
 * @brief Convert byte-writer statistics into the shared recording status columns.
 * Samples written counts complete four-byte payload records; file headers do not
 * count as samples. Byte totals include successfully written/re-written headers.
 * The lock protects only the final copy; no filesystem call occurs while held.
 */
void recordingService::publish()
{
    auto* values = current.values;
    const auto stats = writer.snapshot();
    values[recordingStatus::card] = sink.cardState;
    values[recordingStatus::session] = sink.session;
    values[recordingStatus::part] = sink.part;
    values[recordingStatus::elapsedMs] = stats.elapsedUs / 1000;
    values[recordingStatus::bytesWritten] = stats.bytes + sink.headerBytes;
    values[recordingStatus::samplesWritten] = stats.bytes / 4;
    values[recordingStatus::bufferBytes] = bufferedWriter::capacity;
    values[recordingStatus::usedBytes] = stats.used;
    values[recordingStatus::peakBytes] = stats.peak;
    values[recordingStatus::writePosition] = stats.writePosition;
    values[recordingStatus::readPosition] = stats.readPosition;
    values[recordingStatus::latestDelayUs] = stats.latestUs;
    values[recordingStatus::maxDelayUs] = stats.maxUs;
    values[recordingStatus::maxDelayAtMs] = stats.maxAtUs / 1000;
    values[recordingStatus::maxDelayKind] = stats.maxKind;
    values[recordingStatus::overflows] = stats.overflows;
    // After a write failure, include complete records that never reached the
    // filesystem, plus a possibly partial record. Flush failure alone cannot
    // tell us which accepted bytes the physical card retained.
    values[recordingStatus::lostSamples] = stats.rejectedBytes / 4 +
        (stats.errors ? (stats.unsavedBytes + 3) / 4 : 0);
    values[recordingStatus::writeErrors] = stats.errors;
    const uint32_t now = millis();
    if (now - rateMs >= 500)
    {
        values[recordingStatus::bytesPerSecond] = recording ?
            (stats.bytes - rateBytes) * 1000 / (now - rateMs) : 0;
        rateBytes = stats.bytes; rateMs = now;
    }
    portENTER_CRITICAL(&statusLock);
    published = current;
    portEXIT_CRITICAL(&statusLock);
    lastPublishMs = now;
}

/**
 * @brief Mount once, then service commands and drain full blocks in a permanent loop.
 * The producer may fill RAM while a card write waits. All slow work stays here.
 * Acquisition notices writer failures and detaches its sample interrupt before
 * requesting finish, so no producer is active while we reset or close the ring.
 */
void recordingService::run()
{
    sink.mount();
    sink.space(current.values[recordingStatus::cardBytes], current.values[recordingStatus::freeBytes]);
    publish();
    for (;;)
    {
        const operation action = pending.load(std::memory_order_acquire);
        if (action != operation::none)
        {
            bool success = false;
            if (action == operation::prepare && !recording)
            {
                current.values[recordingStatus::enabled] = 1;
                current.values[recordingStatus::state] = recordingStatus::preparing;
                writer.start(sink, clockUs);
                rateBytes = 0; rateMs = millis();
                publish();
                const uint64_t began = clockUs();
                success = sink.open(argument);
                writer.measure(began, 1, success);
                if (!success)
                {
                    writer.cancel();
                }
                recording = success;
                current.values[recordingStatus::state] = success ? recordingStatus::recording : recordingStatus::failed;
            }
            else if (action == operation::finish)
            {
                success = true;
                if (recording)
                {
                    current.values[recordingStatus::state] = recordingStatus::saving;
                    publish();
                    const bool drained = writer.finish();
                    const uint64_t began = clockUs();
                    const bool closed = sink.close(drained && argument != 0);
                    writer.measure(began, 2, closed);
                    success = drained && closed;
                    recording = false;
                    current.values[recordingStatus::bytesPerSecond] = 0;
                    current.values[recordingStatus::state] = success && argument ? recordingStatus::saved : recordingStatus::failed;
                }
            }
            else if (action == operation::erase && !recording)
            {
                current.values[recordingStatus::state] = recordingStatus::deleting;
                publish();
                success = sink.deleteRecordings(current.values[recordingStatus::deletedFiles]);
                current.values[recordingStatus::state] = success ? recordingStatus::idle : recordingStatus::failed;
            }
            sink.space(current.values[recordingStatus::cardBytes], current.values[recordingStatus::freeBytes]);
            publish();
            result.store(success, std::memory_order_release);
            pending.store(operation::none, std::memory_order_release);
        }
        else if (recording && !writer.failed())
        {
            writer.pump();
        }
        // FAT free-space scans may be slow. Query periodically and count their
        // effect through ring occupancy; write/flush delay measures those calls
        // specifically, not every possible delay in the storage task.
        if (recording && millis() - lastSpaceMs >= 5000)
        {
            sink.space(current.values[recordingStatus::cardBytes], current.values[recordingStatus::freeBytes]);
            lastSpaceMs = millis();
        }
        if (millis() - lastPublishMs >= 100)
        {
            publish();
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
