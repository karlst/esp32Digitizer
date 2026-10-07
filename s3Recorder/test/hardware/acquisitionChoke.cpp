/**
 * @file acquisitionChoke.cpp
 * @brief Temporary Start/Stop behavior for the Choke test; no digitizer calls.
 * runChoke() runs the existing acquisition task on core 0. executeChoke() opens
 * or closes a recording through the existing disk task on core 1. stepChoke()
 * feeds the ring at the scheduled rate and stops on failure. Communications is
 * still independent, so the browser can request Stop while the test runs.
 */
#include "acquisition.h"
#if S3_CHOKE_TEST
#include <esp_timer.h>
#include "chokeEvents.h"

/**
 * @brief Publish generated counts and test results without touching the filesystem.
 * Called only on the acquisition task. Preserve the normal cumulative sample
 * counter across Starts, but reset the test's own counter at each new run.
 * measuredRate is actual generated words/second, not the requested ramp rate.
 */
void acquisition::publishChoke()
{
    current.sampleCount += choke.samples - chokeReportedSamples;
    chokeReportedSamples = choke.samples;
    current.hasSample = choke.samples != 0;
    current.latestRaw = choke.latestSample;
    if (current.running && current.hasSample)
    {
        current.lastSampleMs = millis();
    }
    current.chokeTargetBps = choke.targetBps;
    current.chokeCompletedBps = choke.completedBps;
    current.chokeElapsedMs = choke.elapsedMs;
    current.chokeResult = choke.result;

    // Update actual generation rate at half-second intervals. Stop preserves this
    // last measurement for USB results; Feather already shows zero when stopped.
    const uint32_t now = millis();
    if (current.running && now - rateWindowMs >= 500)
    {
        current.measuredRate = static_cast<uint32_t>((current.sampleCount - rateWindowCount) * 1000 / (now - rateWindowMs));
        rateWindowMs = now;
        rateWindowCount = current.sampleCount;
    }
    publish();
}

/**
 * @brief Stop producing first, then ask the disk task to save queued data and close.
 * reason distinguishes the user's Stop from ring/disk failure or generator lag.
 * A user Stop or successful timed completion marks the file complete. Failed tests retain
 * their accepted samples but mark the final file incomplete. A returned false
 * means draining/finalization reported failure; the generator stays stopped.
 */
bool acquisition::stopChoke(chokeTest::resultCode reason)
{
    chokeEvents::add("stop", "reason,targetBps,completedBps,queuedBytes",
        reason, choke.targetBps, choke.completedBps, storage.buffer().queuedBytes());
    current.running = false;
    choke.stop(reason);
    const bool hadRecording = recordingBuffer != nullptr;
    recordingBuffer = nullptr;
    publishChoke();
    const bool retVal = !hadRecording || storage.finish(reason == chokeTest::stopped);
    if (!retVal && reason == chokeTest::stopped)
    {
        choke.stop(chokeTest::storageLimit);
    }
    publishChoke();

    // finish() has answered, so these published totals include final draining.
    const auto saved = storage.snapshot();
    chokeEvents::add("final", "reason,savedSamples,lostSamples,bytesWritten",
        choke.result, saved.values[recordingStatus::samplesWritten],
        saved.values[recordingStatus::lostSamples], saved.values[recordingStatus::bytesWritten]);
    chokeEvents::add("final-buffer", "peakBytes,remainingBytes,writeErrors,acceptedSamples",
        saved.values[recordingStatus::peakBytes], saved.values[recordingStatus::usedBytes],
        saved.values[recordingStatus::writeErrors], choke.samples);
    return retVal;
}

/**
 * @brief Handle existing Feather/USB commands without inventing a second protocol.
 * Start always records and resets the selected ramp or fixed-rate diagnostic. The dropdown rate is
 * echoed ONLY to satisfy the existing command-confirmation contract; it does not
 * control generation, and the test file's rate is zero (variable). Original
 * commands remain unchanged in history, so retries cannot restart a stopped test.
 */
void acquisition::executeChoke(const acquisitionCommand& command)
{
    uint32_t savedResult = 0;
    if (history.lookup(command, savedResult))
    {
        current.ackId = command.id;
        current.ackResult = savedResult;
    }
    else
    {
        current.ackId = command.id;
        current.ackResult = 0;
        publishChoke();
        bool accepted = false;
        if (command.action == acquisitionCommand::Action::start)
        {
            if (current.running)
            {
                accepted = current.rate == command.rate;
            }
            else
            {
                current.recordingRequested = true;
                publish();

                // Open first; failed preparation must never generate unrecorded data.
                // Zero rate plus the CHOKE header identifies a variable-rate test file.
                chokeEvents::beginRun();
                accepted = storage.prepare(0);
                if (accepted)
                {
                    choke.start(static_cast<uint64_t>(esp_timer_get_time()));
                    chokeEvents::add("generator-start", "targetBps,stepBps,holdMs,wordBits",
                        choke.targetBps, chokeTest::fixedRate ? 0 : 250000, 10000, 32);
                    chokeReportedSamples = 0;
                    current.rate = command.rate;
                    current.running = true;
                    current.error = 0;
                    current.measuredRate = 0;
                    recordingBuffer = &storage.buffer();
                    rateWindowMs = millis();
                    rateWindowCount = current.sampleCount;
                }
                else
                {
                    chokeEvents::add("prepare-failed", "unused");
                }
            }
        }
        else if (command.action == acquisitionCommand::Action::erase)
        {
            // Preserve the existing explicit deletion action; there is never an
            // automatic cleanup on Start. Reject deletion while producing samples.
            accepted = !current.running && !recordingBuffer && storage.erase();
        }
        else
        {
            // Stop and Reboot share orderly saving. Already-stopped requests keep
            // the previous choke outcome instead of changing it to a successful run.
            accepted = !current.running || stopChoke(chokeTest::stopped);
            current.reboot = accepted && command.action == acquisitionCommand::Action::reboot;
        }
        current.ackId = command.id;
        current.ackResult = accepted ? 1 : 2;
        history.remember(command, current.ackResult);
    }
    publishChoke();
}

/**
 * @brief Generate one bounded batch, or finish a run whose writer has failed.
 * No waits for free ring space are allowed: slowing down to match the card would
 * hide the choke point. feed() also detects excessive generator lateness, so that
 * result is reported separately from a card/ring limit.
 */
void acquisition::stepChoke()
{
    if (current.running)
    {
        if (storage.failed())
        {
            stopChoke(chokeTest::storageLimit);
        }
        else
        {
            const uint32_t oldRate = choke.targetBps;
            const bool fed = choke.feed(static_cast<uint64_t>(esp_timer_get_time()), *recordingBuffer);
            if (choke.targetBps != oldRate)
            {
                // Occupancy is live. Speed is the last published storage window;
                // it can be stale if the disk task is stuck in a long operation.
                const auto saved = storage.snapshot();
                chokeEvents::add("rate-change", "oldBps,newBps,queuedBytes,lastWriteBps",
                    oldRate, choke.targetBps, recordingBuffer->queuedBytes(),
                    saved.values[recordingStatus::bytesPerSecond] * 8);
            }
            if (!fed)
            {
                stopChoke(choke.result);
            }
            else if (millis() - lastPublishMs >= 10)
            {
                publishChoke();
            }
        }
    }
}

/**
 * @brief Run the synthetic source on core 0, leaving normal disk pumping on core 1.
 * There is deliberately no adc.begin() or DRDY interrupt attachment in this build.
 * Sleeping one RTOS tick between short batches lets commands and system tasks run.
 * The elapsed-time schedule, rather than tick count, determines the offered rate.
 */
void acquisition::runChoke()
{
    current.ready = true;
    current.error = 0;
    publishChoke();
    for (;;)
    {
        acquisitionCommand command;
        if (!current.reboot && xQueueReceive(commands, &command, 0) == pdTRUE)
        {
            executeChoke(command);
        }
        stepChoke();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
#endif
