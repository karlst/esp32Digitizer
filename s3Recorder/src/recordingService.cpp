/**
 * @file recordingService.cpp
 * @brief Perform slow filesystem work on core 1, while samples arrive on core 0.
 * Read request() for command handoff, run() for disk operations, and publish()
 * for the statistics sent to Feather. The communications loop shares core 1 but
 * is a separate task: an SD wait does not hold an acquisition/status mutex.
 *
 * There are two different synchronization jobs here. The atomic mailbox safely
 * transfers one command between cores. statusLock protects copies of the larger
 * display record. Neither is the ring-buffer synchronization: bufferedWriter
 * has its own one-producer/one-consumer positions for that purpose.
 */
#include "recordingService.h"
#include <Arduino.h>
#include <esp_timer.h>
#include "chokeEvents.h"

/**
 * @brief Supply a 64-bit microsecond clock, avoiding millis() rollover in long runs.
 */
uint64_t recordingService::clockUs()
{
    const uint64_t retVal = static_cast<uint64_t>(esp_timer_get_time());
    return retVal;
}

/**
 * @brief Launch the storage worker; mounting a card happens asynchronously there.
 * acquisition::begin() calls this during board startup. A FreeRTOS task is a
 * background thread; this creates the disk thread but does not start recording.
 * @return True means the thread was created, NOT that the card mounted. The
 * thread tries mounting later and reports card status separately.
 */
bool recordingService::begin()
{
    TaskHandle_t worker = nullptr;
    // Arguments: entry function, debug name, 6144-byte stack for local variables,
    // this object, priority 1, returned task handle, CPU core 1. The ADC thread
    // uses core 0. The temporary handle is not needed again; the task keeps running.
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
 *
 * The mailbox consists of pending (operation), argument (rate or completion
 * flag), and result (success/failure). There is one caller, the acquisition task,
 * and at most one outstanding request. This is not a queue for multiple clients.
 * Writing argument BEFORE publishing pending ensures the storage task sees the
 * correct argument. Storage publishes result BEFORE clearing pending; seeing
 * none again tells this caller both that work finished and that result is valid.
 * @return False if no worker exists, another operation is pending, or the disk
 * operation fails. A true result means completed work, not merely a queued request.
 */
bool recordingService::request(operation action, uint32_t value)
{
    bool retVal = available && pending.load(std::memory_order_acquire) == operation::none;
    if (retVal)
    {
        // Store ordinary argument memory first, then announce the request. release
        // and acquire pair across cores so the operation cannot use an older argument.
        argument = value;
        pending.store(action, std::memory_order_release);
        while (pending.load(std::memory_order_acquire) != operation::none)
        {
            // Sleep for an RTOS tick instead of occupying the CPU while disk work runs.
            // The ADC is already stopped here, so this does not drop incoming samples.
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        retVal = result.load(std::memory_order_acquire);
    }
    return retVal;
}

/**
 * @brief Open a new file before acquisition starts; false leaves sampling stopped.
 * Called by acquisition::execute() for Start with recording checked. rate is
 * samples per second and goes into the file header. The acquisition task waits
 * here; the storage thread opens the file. Only a true result allows ADC startup.
 */
bool recordingService::prepare(uint32_t rate)
{
    const bool retVal = request(operation::prepare, rate);
    return retVal;
}

/**
 * @brief Drain and close after the producer stops; complete=false marks a faulted run.
 * Called after acquisition stops producing samples, on Stop/Reboot or a fault.
 * complete says whether acquisition ended normally. Even if every queued byte
 * is saved, complete=false makes the file/UI report an incomplete recording.
 * The return value reports drain/close success; it can be true for a deliberately
 * incomplete file. The caller and displayed state also retain the acquisition fault.
 */
bool recordingService::finish(bool complete)
{
    const bool retVal = request(operation::finish, complete ? 1 : 0);
    return retVal;
}

/**
 * @brief Execute the explicitly requested recording-file deletion while stopped.
 * acquisition::execute() first checks that sampling is stopped. The storage
 * thread additionally refuses deletion while its file is active. This waits for
 * the explicit deletion operation; it is never called automatically on Start.
 */
bool recordingService::erase()
{
    const bool retVal = request(operation::erase, 0);
    return retVal;
}

/**
 * @brief Return the producer interface; acquisition attaches it only during recording.
 * Returns the owned byte writer by reference, not a copy of its 64-KB array.
 * Acquisition enables that reference only after prepare succeeds and disables
 * its use before finish. The producer calls submit(), not pump() or snapshot().
 */
bufferedWriter& recordingService::buffer()
{
    return writer;
}

/**
 * @brief Check the writer's atomic fault flag without reading its live statistics.
 * Called by acquisition while running. This reads the generic writer's shared
 * failure flag; it does not query the card and therefore cannot stall ADC work.
 */
bool recordingService::failed() const
{
    const bool retVal = writer.failed();
    return retVal;
}

/**
 * @brief Copy a previously published storage snapshot under a short memory-only lock.
 * Called by acquisition::snapshot() on the communications side. The disk task
 * may be replacing a 64-bit counter while this 32-bit CPU reads it, so a short
 * critical section protects the entire copy. It never covers SD operations.
 * The returned copy may be about 100 ms old (or older while a disk call blocks).
 * That is appropriate for display; it must not control individual sample reads.
 */
recordingStatus recordingService::snapshot()
{
    // Only copy shared memory while locked. No file writes, prints or waits may
    // be added inside this critical section; readers need a quick snapshot.
    portENTER_CRITICAL(&statusLock);
    const recordingStatus retVal = published;
    portEXIT_CRITICAL(&statusLock);
    return retVal;
}

/**
 * @brief Recover the service object for FreeRTOS's ordinary-function entry point.
 * FreeRTOS passes the void* supplied by begin(). Convert it back to our object
 * and enter run(), which never returns. No second recordingService is created.
 */
void recordingService::taskEntry(void* argument)
{
    static_cast<recordingService*>(argument)->run();
}

/**
 * @brief Convert byte-writer statistics into the shared recording status columns.
 * Samples written counts complete four-byte payload records; file headers do not
 * count as samples. Byte totals include successfully written/re-written headers.
 * The lock protects only the final copy; no filesystem call occurs while held.
 *
 * Called only by the disk thread, after commands and roughly every 100 ms.
 * It updates a memory snapshot, NOT the UART. featherLink sends its own copy
 * roughly every 500 ms. The indexed array order is defined in recordingStatus.h.
 * The generic writer measures bytes; dividing by four is this application's
 * current sample format. A future format change must update that translation.
 */
void recordingService::publish()
{
    auto* values = current.values;
    const auto stats = writer.snapshot();
    values[recordingStatus::card] = sink.cardState;
    // During recording use only cached file growth. Never scan here: publish()
    // runs while the producer can be filling the ring on the other core.
    if (recording && spaceEstimateValid)
    {
        values[recordingStatus::freeBytes] = spaceEstimate.remaining(sink.allocatedBytes);
        values[recordingStatus::freeSpaceMode] = recordingStatus::spaceEstimated;
    }
    values[recordingStatus::session] = sink.session;
    values[recordingStatus::part] = sink.part;
    // Convert microseconds to milliseconds only for fields defined that way on
    // the wire. Byte count includes adapter headers; sample count excludes them.
    values[recordingStatus::elapsedMs] = stats.elapsedUs / 1000;
    values[recordingStatus::bytesWritten] = stats.bytes + sink.headerBytes;
    values[recordingStatus::samplesWritten] = stats.bytes / 4;
    // Positions and occupancy are BYTES within the RAM ring, not SD file offsets.
    // Peak comes from the producer, so brief peaks between UART reports survive.
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
    // Compute recent payload throughput over an elapsed half-second-or-longer
    // window. It is actual accepted bytes/time, not the requested ADC rate.
    const uint32_t now = millis();
    if (now - rateMs >= 500)
    {
        if (recording)
        {
            values[recordingStatus::bytesPerSecond] = (stats.bytes - rateBytes) * 1000 / (now - rateMs);
        }
#if !S3_CHOKE_TEST
        else
        {
            values[recordingStatus::bytesPerSecond] = 0;
        }
#endif
        // Choke-test builds retain the last measured write speed after Stop/failure.
        // It is a completed measurement window, not the requested generator rate.
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
 *
 * This is the permanent disk thread created in begin(). Its sequence is:
 * 1. Try the existing card filesystem and publish readiness, while ADC stays idle.
 * 2. If a command is present, finish that operation and answer the waiting caller.
 * 3. Otherwise, if recording, pump one queued block from RAM into the file.
 * 4. Publish cached/estimated statistics and briefly yield CPU time; no active scan.
 *
 * The recording boolean means a recording file is active, including the short
 * interval after opening before ADC startup. It does not itself start the ADC.
 * On writer failure this loop stops normal pumping; acquisition sees failed(),
 * stops the producer, then requests finish so earlier good bytes can be drained.
 * While a long storage call is blocked, the communications thread can still send
 * the last published Preparing/Saving/Deleting state; those are pending, not success.
 */
void recordingService::run()
{
    // Startup only discovers readiness. A mount failure is visible but does not
    // prevent monitoring the ADC; a later recording Start retries mounting.
    sink.mount();
    refreshSpace();
    publish();
    for (;;)
    {
        // Check the acquisition-to-storage mailbox. acquire pairs with request()
        // and makes the associated non-atomic argument safe to read.
        const operation action = pending.load(std::memory_order_acquire);
        if (action != operation::none)
        {
            bool success = false;
            if (action == operation::prepare && !recording)
            {
                current.values[recordingStatus::fileOpenUs] = 0;
                spaceEstimateValid = false;
                current.values[recordingStatus::enabled] = 1;
                current.values[recordingStatus::state] = recordingStatus::preparing;
                // Reset this recording before measuring file preparation. No producer is
                // enabled yet, so resetting positions cannot erase queued live samples.
                writer.start(sink, clockUs);
                rateBytes = 0; rateMs = millis();
                current.values[recordingStatus::bytesPerSecond] = 0;
                publish();
                const uint64_t began = clockUs();
                // argument is the requested sample rate. Opening includes an initial
                // header and synchronization. Acquisition cannot start until this succeeds.
                success = sink.open(argument);
                current.values[recordingStatus::fileOpenUs] = clockUs() - began;
#if S3_CHOKE_TEST
                chokeEvents::add("file-open", "durationUs,success,unused,unused", clockUs() - began, success);
#endif
                if (!success)
                {
                    // No file is ready and no samples were submitted. Freeze the failed
                    // attempt duration without trying to flush a nonexistent recording.
                    writer.measure(clockUs(), 1, false);
                    writer.cancel();
                }
                // Scan after opening so the baseline includes the initial header
                // and directory entry. Acquisition is still waiting for our reply.
                refreshSpace();
                if (success)
                {
                    spaceEstimateValid = current.values[recordingStatus::freeSpaceMode] == recordingStatus::spaceMeasured;
                    spaceEstimate.begin(current.values[recordingStatus::freeBytes], sink.allocatedBytes);
                    // Start the recording clock and delay maximum AFTER opening and
                    // scanning. No producer exists yet, so this reset discards no data.
                    writer.start(sink, clockUs);
                    rateBytes = 0; rateMs = millis();
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
                    // First save queued bytes and synchronize them. Then let the file adapter
                    // write the final header, close, and check it. Both stages can fail.
#if S3_CHOKE_TEST
                    const uint64_t drainBegan = clockUs();
                    chokeEvents::add("drain-start", "queuedBytes", writer.queuedBytes());
#endif
                    const bool drained = writer.finish();
#if S3_CHOKE_TEST
                    chokeEvents::add("drain-end", "durationUs,remainingBytes,success,unused",
                        clockUs() - drainBegan, writer.queuedBytes(), drained);
#endif
                    const uint64_t began = clockUs();
                    const bool closed = sink.close(drained && argument != 0);
                    writer.measure(began, 2, closed);
#if S3_CHOKE_TEST
                    chokeEvents::add("file-close", "durationUs,success,unused,unused", clockUs() - began, closed);
#endif
                    // An acquisition fault can still permit successful saving of earlier
                    // samples. Only a normal completion flag AND successful saving show Saved.
                    success = drained && closed;
                    recording = false;
#if !S3_CHOKE_TEST
                    current.values[recordingStatus::bytesPerSecond] = 0;
#endif
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
            // Invalid commands must not trigger a scan while another recording is
            // active. prepare already scanned before enabling production.
            if (!recording && action != operation::prepare)
            {
                refreshSpace();
            }
            publish();
            // Publish final status/result BEFORE marking the mailbox idle. The waiting
            // acquisition task then sends its final acknowledgement through featherLink.
            result.store(success, std::memory_order_release);
            pending.store(operation::none, std::memory_order_release);
        }
        else if (recording && !writer.failed())
        {
            // This is the regular consumer call. It reads the ring and may block on
            // one disk write; ADC interrupts keep appending to other ring space.
#if S3_CHOKE_TEST
            const uint64_t began = clockUs();
            const uint32_t before = writer.queuedBytes();
#endif
            writer.pump();
#if S3_CHOKE_TEST
            // Only regular pumps over 50 ms produce detail events. finish() owns
            // its internal writes, so final draining is timed as a whole above.
            const uint64_t duration = clockUs() - began;
            if (duration >= 50000)
            {
                chokeEvents::add("slow-pump", "durationUs,beforeBytes,afterBytes,failed",
                    duration, before, writer.queuedBytes(), writer.failed(), false);
            }
#endif
        }
        if (millis() - lastPublishMs >= 100)
        {
            publish();
        }
        // Yield so communications and idle tasks on core 1 can run. Sampling
        // continues on core 0; this delay does not set the digitizer sample rate.
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/**
 * @brief Refresh free-space totals on the disk task, timing the entire scan.
 * Called at startup, before allowing a new recording, and after Stop/deletion.
 * Never call with a sample producer active: a scan blocks this task from pumping.
 * A failed scan marks retained totals stale instead of presenting them as current.
 */
void recordingService::refreshSpace()
{
#if S3_CHOKE_TEST
    const uint64_t began = clockUs();
    const uint32_t before = writer.queuedBytes();
    chokeEvents::add("space-start", "queuedBytes,recording,unused,unused", before, recording);
#endif
    const bool measured = sink.space(current.values[recordingStatus::cardBytes], current.values[recordingStatus::freeBytes]);
    current.values[recordingStatus::freeSpaceMode] = measured ? recordingStatus::spaceMeasured : recordingStatus::spaceStale;
#if S3_CHOKE_TEST
    chokeEvents::add("space-end", "durationUs,beforeBytes,afterBytes,freeBytes",
        clockUs() - began, before, writer.queuedBytes(), current.values[recordingStatus::freeBytes]);
#endif
}
