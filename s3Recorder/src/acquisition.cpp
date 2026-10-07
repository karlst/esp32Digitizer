/**
 * @file acquisition.cpp
 * @brief Run sample collection in a background task, separate from communications.
 *
 * ADC means analog-to-digital converter: our ADS1256 digitizer. A sample is
 * one measured value. DRDY means Data Ready, the wire on S3 pin 9 that goes
 * from high to low whenever the digitizer has a new sample available.
 *
 * Follow the program in this order:
 * 1. begin() creates a command queue and a FreeRTOS task (a background thread).
 * 2. taskEntry() starts run(), which initializes the digitizer and waits.
 * 3. Communications code calls submit() to request Start, Stop, or Reboot.
 * 4. run() takes that command from the queue and calls execute().
 * 5. Below 30k, collect() waits for samples and calls adc.read() in ads1256.cpp.
 *    At 30k, readyInterrupt() calls fastCapture::onReady() to read immediately;
 *    collectFast() copies its latest sample and totals into ordinary status.
 *
 * The task runs on CPU core 0 and is the only task allowed to operate the
 * digitizer. Below 30k the interrupt counts events and wakes this task. At 30k
 * it performs the short SPI read itself; the task detaches that interrupt before
 * sending commands, so both paths cannot use SPI at the same time.
 * The communications loop on the other core submits commands and reads status;
 * it never reaches into the digitizer while a sample is being read.
 *
 * current is the acquisition task's working status. publish() copies it into
 * published, and snapshot() gives communications a protected copy of published.
 * Monitoring keeps the latest value/count. Optional recording sends EVERY accepted
 * sample through sampleFormatter to the RAM buffer, then the storage task writes it.
 * The temporary S3_CHOKE_TEST build substitutes acquisitionChoke.cpp for run()
 * and execute(): synthetic samples enter the same ring, with no digitizer calls.
 */
#include "acquisition.h"

/**
 * @brief Create the command queue and launch the background acquisition task.
 *
 * The queue holds one pending Start, Stop, or Reboot command. The new task
 * initializes the digitizer but waits for Start before collecting samples.
 * @return True if the queue and task were created. This does NOT confirm that
 * the digitizer initialized; the task reports that separately through status.
 */
bool acquisition::begin()
{
    // Allocate space for one command, copied by value. The queue safely moves
    // commands from the communications task to this task, even across CPU cores.
    // A null handle means allocation failed.
    storage.begin(); // Card failure must not prevent acquisition-only operation.
    commands = xQueueCreate(1, sizeof(acquisitionCommand));

    // Only try to create the task if the queue exists (the && enforces this).
    // Arguments: entry function; debugging name; 4096-byte stack for calls and
    // local variables; this object as the entry argument; scheduling priority 3;
    // where to store the task handle; CPU core 0. pdPASS means creation succeeded.
    // The new task can start executing before begin() returns.
    const bool retVal = commands && xTaskCreatePinnedToCore(taskEntry, "adcReader", 4096,
        this, 3, &worker, 0) == pdPASS;

    // If startup failed, release any queue we created and expose an initialization
    // error (code 1). publish() updates shared status; it sends no serial message.
    if (!retVal)
    {
        if (commands)
        {
            vQueueDelete(commands);
            commands = nullptr;
        }
        current.error = 1;
        publish();
    }
    return retVal;
}

/**
 * @brief Give the acquisition task a command to execute when it next checks its queue.
 * @param command Already validated Start, Stop, or Reboot request, including its ID.
 * @return True if a copy was queued, not proof that the command has executed.
 * False means there is no queue or its single pending-command slot is full.
 */
bool acquisition::submit(const acquisitionCommand& command)
{
    // The final zero means "do not wait for space." Keep communications responsive
    // if acquisition is busy; never overwrite a command that is already waiting.
    const bool retVal = commands && xQueueSend(commands, &command, 0) == pdTRUE;
    return retVal;
}

/**
 * @brief Give the caller a consistent copy of the last published acquisition status.
 * @return A separate status object the caller can format/transmit after unlocking.
 * It may lag the working status by about 10 milliseconds during acquisition.
 */
acquisitionStatus acquisition::snapshot()
{
    // Another core could be replacing published while we copy it. Without a
    // lock we could mix an old sample with a new count, or read only half of an
    // updated 64-bit count on this 32-bit processor. This short critical-section
    // lock lets only one core copy published at a time. It is not a sample/SPI
    // lock and is released before any formatting, transmission, or waiting.
    portENTER_CRITICAL(&snapshotLock);
    acquisitionStatus retVal = published;
    portEXIT_CRITICAL(&snapshotLock);
    retVal.recording = storage.snapshot();
    retVal.recording.values[recordingStatus::enabled] = retVal.recordingRequested ? 1 : 0;
    return retVal;
}

/**
 * @brief Copy working status into the shared status used by communications.
 *
 * Called after commands and faults, and about every 10 milliseconds while
 * collecting. "Publish" here means copy in memory, not send to the Feather.
 * The communications code decides when to transmit its own snapshot.
 */
void acquisition::publish()
{
    // Use the same lock as snapshot() so nobody sees a partly replaced record.
    // Keep the protected work to one structure copy; do no hardware I/O here.
    portENTER_CRITICAL(&snapshotLock);
    published = current;
    portEXIT_CRITICAL(&snapshotLock);
    lastPublishMs = millis();
}

/**
 * @brief Start this object's run() method when FreeRTOS launches the task.
 * @param argument The acquisition object passed as "this" in begin().
 *
 * FreeRTOS calls an ordinary function with an untyped pointer. This static
 * function converts that pointer back to an acquisition object so it can call
 * the object's member function. run() loops indefinitely; it does not return.
 */
void acquisition::taskEntry(void* argument)
{
    static_cast<acquisition*>(argument)->run();
}

/**
 * @brief Handle the digitizer's signal that a new sample is ready.
 * @param argument The acquisition object supplied when the interrupt was attached.
 *
 * An edge is a voltage-level change. We attach this function only to FALLING
 * edges: DRDY changing from high to low. This interrupt briefly interrupts
 * normal task execution. At 30k it performs the bounded read immediately; at
 * lower rates it only wakes the task. Neither path prints or allocates memory.
 * IRAM_ATTR places this function in the S3's internal instruction RAM.
 */
void IRAM_ATTR acquisition::readyInterrupt(void* argument)
{
    // Recover our object and count this event. collect() compares this count
    // before/after reading to detect another sample arriving during the read.
    acquisition* self = static_cast<acquisition*>(argument);
    if (self->fastMode)
    {
        self->capture.onReady();
        return;
    }
    self->readyUs = micros();
    ++self->readyEdges;

    // A task notification is a wake-up signal, not the sample itself. Use the
    // FromISR version because we are inside an interrupt service routine (ISR).
    // FreeRTOS sets wake if a higher-priority task should run immediately.
    BaseType_t wake = pdFALSE;
    vTaskNotifyGiveFromISR(self->worker, &wake);

    // Ask the scheduler to run that task as we leave the interrupt.
    if (wake == pdTRUE)
    {
        portYIELD_FROM_ISR();
    }
}

/**
 * @brief Stop acquisition after a read failure and make the error visible in status.
 * @param error Status error code: 2 for no ready sample within the timeout,
 * or 3 for a failed/possibly inconsistent sample read.
 *
 * Preserve the previous accepted sample, count, and detailed fault evidence.
 * Clear ready so another Start is rejected; reboot is the recovery path.
 */
void acquisition::fault(uint32_t error)
{
    // Stop new wake-up interrupts, then ask the digitizer to stop. Keep the
    // original error even if that stop attempt also fails.
    detachInterrupt(ads1256::readyPin);
    fastMode = false;
    if ((current.readDetail & 255) == 4)
    {
        adc.abandon();
    }
    else
    {
        adc.stop();
    }

    // Report that software collection has stopped; this does not guarantee
    // an unresponsive digitizer actually entered standby.
    current.running = false;
    current.ready = false;
    current.measuredRate = 0;
    current.error = error;

    // Stop the producer before draining. Preserve good queued samples, but mark
    // the file incomplete because acquisition ended with a hardware error.
    capture.setWriter(nullptr);
    if (recordingBuffer)
    {
        recordingBuffer = nullptr;
        publish();
        storage.finish(false);
    }
    publish();
}

/**
 * @brief Execute one validated command taken from the queue by run().
 * @param command Start, Stop, or Reboot request, with its rate and request ID.
 *
 * The acquisition task alone calls this function, so commands cannot operate
 * SPI at the same time as collect(). ackId identifies the request answered;
 * ackResult is 1 for accepted or 2 for rejected. These fields are published
 * for communications to send back; this function does not send the reply.
 * Choke-test builds delegate to executeChoke(), which preserves command IDs but
 * replaces ADC operation with a timed synthetic source. See acquisitionChoke.cpp.
 */
void acquisition::execute(const acquisitionCommand& command)
{
#if S3_CHOKE_TEST

    // The temporary throughput build never initializes, reads or stops the ADC.
    executeChoke(command);
#else

    // Check the bounded history of recent requests before touching hardware.
    // A duplicate should get its recorded answer, not execute twice. In
    // particular, a repeated old Start must not undo a subsequent Stop while
    // that request is still in history. History is not retained across reboot.
    uint32_t savedResult = 0;
    if (history.lookup(command, savedResult))
    {
        current.ackId = command.id;
        current.ackResult = savedResult;
    }
    else
    {
        bool accepted = false;

        // Publish a pending acknowledgement before potentially slow file work.
        // Communications remains alive while this task waits for storage.
        current.ackId = command.id;
        current.ackResult = 0;
        publish();
        if (command.action == acquisitionCommand::Action::start)
        {
            // While running, accept Start only if it asks for the same rate.
            // Changing rate requires Stop then Start; do not reconfigure mid-read.
            if (current.running)
            {
                accepted = current.rate == command.rate && command.record == (recordingBuffer != nullptr);
            }

            // A stopped digitizer must have initialized successfully and have
            // no outstanding fault. Otherwise leave this request rejected.
            else if (current.ready)
            {
                // Configure/calibrate the actual chip before saying Start worked.
                current.recordingRequested = command.record;
                publish();
                const bool fileReady = !command.record || storage.prepare(command.rate);
                accepted = fileReady && adc.start(command.rate);
                if (accepted)
                {
                    // Record applied settings and clear old read diagnostics.
                    // Keep cumulative sampleCount and the previous sample;
                    // Start does not erase measurements from earlier runs.
                    current.rate = command.rate;
                    current.running = true;
                    current.error = 0;
                    current.readFault = 0;
                    current.readDetail = 0;
                    current.wakeUs = 0;
                    current.spiUs = 0;
                    current.maxWakeUs = 0;
                    current.maxReadUs = 0;
                    current.observedEdges = 0;
                    current.maxGapUs = 0;
                    current.measuredRate = 0;

                    // Remove wake-up signals left from the previous run (true
                    // clears the notification count; zero means do not wait).
                    // Reset edge tracking, then enable the high-to-low DRDY
                    // interrupt. Pass this object to readyInterrupt().
                    ulTaskNotifyTake(pdTRUE, 0);
                    readyEdges = 0;
                    readyUs = 0;
                    awaitingFirstEdge = command.rate > 2000;
                    fastMode = command.rate == 30000;
                    recordingBuffer = command.record ? &storage.buffer() : nullptr;
                    capture.setWriter(recordingBuffer);
                    capture.reset();
                    lastFastCount = 0;
                    previousEdge = 0;
                    attachInterruptArg(ads1256::readyPin, readyInterrupt, this, FALLING);

                    // Start the no-sample timeout and measured-rate window now.
                    // At high rates, ignore a sample that predates interrupt
                    // attachment: its remaining read window is unknown. The
                    // first observed falling edge begins this run's collection.
                    lastReadMs = millis();
                    rateWindowMs = lastReadMs;
                    rateWindowCount = current.sampleCount;
                }
                else
                {
                    // Chip setup failed. Reject Start and require recovery.
                    if (fileReady)
                    {
                        current.ready = false;
                        current.error = 3;
                        if (command.record)
                        {
                            storage.finish(false);
                        }
                    }
                }
            }
        }
        else if (command.action == acquisitionCommand::Action::erase)
        {
            // Recheck on S3; a disabled browser button is not an authorization
            // boundary. Never delete while the ADC or file producer is active.
            accepted = !current.running && !recordingBuffer && storage.erase();
        }
        else
        {
            // Both Stop and Reboot first stop sample collection. This else
            // handles those two actions because the parser rejects unknown ones.
            detachInterrupt(ads1256::readyPin);
            if (fastMode)
            {
                // Fold in the final accepted samples before reporting Stop.
                collectFast();
                fastMode = false;
            }
            const bool stopped = adc.stop();
            current.running = false;
            current.measuredRate = 0;

            // Keep a hardware timeout visible even though software has stopped.
            if (!stopped)
            {
                current.ready = false;
                current.error = 2;
            }

            // Accept the software Stop even if the chip did not answer. For
            // Reboot, set a flag: communications sends the reply before actually
            // restarting the S3. Do not reboot here and lose that confirmation.
            capture.setWriter(nullptr);
            accepted = true;
            if (recordingBuffer)
            {
                recordingBuffer = nullptr;
                publish();
                accepted = storage.finish(stopped && current.error == 0);
            }
            current.reboot = accepted && command.action == acquisitionCommand::Action::reboot;
        }

        // Save this request's result for status reporting and duplicate detection.
        current.ackId = command.id;
        current.ackResult = accepted ? 1 : 2;
        history.remember(command, current.ackResult);
    }
    publish();
#endif
}

/**
 * @brief Initialize the digitizer, then repeatedly handle commands and collect samples.
 *
 * This is the background task's permanent loop, entered through taskEntry().
 * Initialization leaves the digitizer in standby. Only a successful Start
 * command sets current.running and enables collection.
 * With S3_CHOKE_TEST, runChoke() replaces this loop and never initializes the ADC.
 */
void acquisition::run()
{
#if S3_CHOKE_TEST
    runChoke();
#else

    // Hardware setup happens here, after acquisition::begin() creates the task.
    // Report its result separately from whether task creation itself succeeded.
    current.ready = adc.begin();
    current.error = current.ready ? 0 : 1;
    publish();
    for (;;)
    {
        // Check for one queued command without waiting (final argument zero).
        // Do this before the next sample so Stop can be serviced promptly.
        // Once Reboot is accepted, leave further commands for the restart.
        acquisitionCommand command;
        if (!current.reboot && xQueueReceive(commands, &command, 0) == pdTRUE)
        {
            execute(command);
        }

        // collect() handles at most one sample per call and bounds its wait.
        // Returning here allows another command check between read attempts.
        if (current.running && recordingBuffer && storage.failed())
        {
            // Ring overflow or disk failure stops the producer promptly. Keep
            // ADC readiness: storage trouble does not mean the digitizer broke.
            detachInterrupt(ads1256::readyPin);
            if (fastMode)
            {
                collectFast();
            }
            fastMode = false;
            capture.setWriter(nullptr);
            const bool stopped = adc.stop();
            current.running = false;
            current.measuredRate = 0;
            if (!stopped)
            {
                current.ready = false;
                current.error = 2;
            }
            recordingBuffer = nullptr;
            publish();
            storage.finish(false);
        }
        if (current.running)
        {
            if (fastMode)
            {
                collectFast();

                // ISR capture continues during sleep. This gives idle/system tasks
                // CPU time instead of a busy task starving the core at 30k.
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            else
            {
                collect();
            }
        }
        else
        {
            // While stopped, sleep five milliseconds rather than spinning through
            // an empty queue. Other tasks can use the CPU during this sleep.
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
#endif
}

/**
 * @brief Wait briefly for one sample, read it, and update counts and status.
 *
 * Called only by run() in the acquisition task. The DRDY interrupt wakes us;
 * adc.read() in ads1256.cpp performs the actual SPI transfer. We check the
 * interrupt count around that transfer to detect a new sample becoming ready
 * before the previous read finishes. Such a read is rejected as uncertain.
 * When recording is enabled, each accepted value is also queued in the byte writer.
 */
void acquisition::collect()
{
    if (current.running)
    {
        // DRDY low means a sample is already waiting, so read without sleeping.
        // Otherwise sleep until notified, or until the 20-millisecond wait ends.
        // Calibration can leave DRDY low before we attach the interrupt. At high
        // rates that old sample has unknown remaining read time. Establish this
        // run's boundary at a newly observed ready event instead; the pre-boundary
        // calibration sample is not an acquired or rejected sample in this run.
        if (digitalRead(ads1256::readyPin) != LOW || (awaitingFirstEdge && readyEdges == 0))
        {
            // Clear accumulated notifications on wake. A notification only says
            // "check the device"; recheck the pin below before reading. The
            // timeout lets run() check commands even if no more samples arrive.
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
        }
        if (digitalRead(ads1256::readyPin) == LOW && (!awaitingFirstEdge || readyEdges != 0))
        {
            awaitingFirstEdge = false;

            // Save how many ready interrupts have occurred. The difference from
            // the last accepted read reveals observed events we did not service.
            // unsigned subtraction also handles the 32-bit counter wrapping.
            const uint32_t edgeBefore = readyEdges;
            const uint32_t elapsedEdges = edgeBefore - previousEdge;

            // Count ready events already missed BEFORE this read, even if this
            // read subsequently fails. Do not mix them with rejected read attempts.
            if (elapsedEdges > 1)
            {
                current.missedEdges += elapsedEdges - 1;
            }
            int32_t sample = 0;

            // Split the observed latency into task wake delay and driver read time.
            // No observed edge means startup already found the pin low; UINT32_MAX
            // marks an unknown delay rather than pretending it was immediate.
            current.wakeUs = edgeBefore ? micros() - readyUs : UINT32_MAX;
            const bool complete = adc.read(sample);

            // Save this observation once so the decision and reason counters agree.
            const bool overlapped = readyEdges != edgeBefore;
            current.observedEdges = readyEdges;
            current.readDetail = adc.readDiagnostic();
            current.spiUs = adc.transferMicros();
            if (current.wakeUs != UINT32_MAX && current.wakeUs > current.maxWakeUs)
            {
                current.maxWakeUs = current.wakeUs;
            }
            if ((current.readDetail >> 8) > current.maxReadUs)
            {
                current.maxReadUs = current.readDetail >> 8;
            }

            // Accept only if the driver succeeded AND no new ready interrupt
            // occurred during the read. Otherwise the three received bytes might
            // not belong to one sample, so stop rather than record suspect data.
            if (complete && !overlapped)
            {
                // Count this accepted sample, replace the displayed raw number,
                // and remember when it arrived for freshness/timeout reporting.
                if (recordingBuffer)
                {
                    sampleFormatter::submit(*recordingBuffer, sample);
                }
                previousEdge = edgeBefore;
                ++current.sampleCount;
                current.latestRaw = sample;
                current.hasSample = true;
                current.lastSampleMs = millis();
                lastReadMs = current.lastSampleMs;
            }
            else
            {
                // Preserve evidence before stopping: bit 0 (value 1) means the
                // driver failed; bit 1 (value 2) means another ready interrupt
                // occurred during the read. Both together give value 3.
                // The driver's separate diagnostic includes its stage and time.
                // Count one rejected attempt, then each reason that applied.
                // One read can fail both checks; adding the reason counts would
                // double-count it. None of these estimates total samples lost.
                ++current.rejectedReads;
                if (!complete)
                {
                    ++current.readFailures;
                }
                if (overlapped)
                {
                    ++current.overlapReads;
                }
                current.readFault = (complete ? 0U : 1U) | (overlapped ? 2U : 0U);
                current.readDetail = adc.readDiagnostic();
                fault(3);
            }
        }
        else if (millis() - lastReadMs >= 100)
        {
            // No ready sample for at least 100 milliseconds: stop and report
            // timeout. This catches a silent digitizer or stuck-high DRDY wire.
            // Count one timeout incident, not an invented number of lost samples.
            ++current.readyTimeouts;
            fault(2);
        }

        // About once a second, calculate actual successful reads per second:
        // samples added since the last measurement * 1000 / elapsed milliseconds.
        // This reports what we received, not merely the rate we asked the chip for.
        const uint32_t elapsedMs = millis() - rateWindowMs;
        if (current.running && elapsedMs >= 1000)
        {
            current.measuredRate = static_cast<uint32_t>(
                (current.sampleCount - rateWindowCount) * 1000 / elapsedMs);
            rateWindowCount = current.sampleCount;
            rateWindowMs += elapsedMs;
        }

        // Refresh shared status about every 10 milliseconds, avoiding a cross-core
        // copy for every sample. Actual transmission is handled elsewhere.
        if (millis() - lastPublishMs >= 10)
        {
            publish();
        }
    }
}

/**
 * @brief Merge the interrupt reader's latest result and totals into normal status.
 *
 * This task does not read SPI in fast mode. It copies an internally consistent
 * capture record, accounts for all accepted reads since the last copy, and keeps
 * the same rate/freshness/fault behavior as the low-rate path. No samples are
 * lost merely because this task wakes less often: the ISR counts each accepted read.
 * The ISR itself queues each recorded value; this slower snapshot is only for status.
 */
void acquisition::collectFast()
{
    // Copy under a short same-core interrupt mask, then release it before doing
    // arithmetic or publishing across cores. Counts include every accepted read,
    // even when several interrupts ran between worker visits.
    const fastCapture::captureSnapshot result = capture.snapshot();
    if (result.count != lastFastCount)
    {
        current.sampleCount += result.count - lastFastCount;
        lastFastCount = result.count;
        current.latestRaw = result.raw;
        current.hasSample = true;
        current.lastSampleMs = millis() - (micros() - result.sampleUs) / 1000;
        lastReadMs = current.lastSampleMs;
    }

    // These timings describe ISR work, not a task wake or library transfer.
    // Zero wake/spi values at 30k mean those stages are absent, not free SPI.
    current.observedEdges = result.events;
    current.wakeUs = 0;
    current.spiUs = 0; // No separately timed library call in the direct-register path.
    current.readDetail = (result.readUs << 8) | result.stage;
    current.maxReadUs = result.maxReadUs;
    current.maxGapUs = result.maxGapUs;
    if (result.fault && current.readFault == 0)
    {
        // Capture freezes at its first bad read. Account for that attempt once,
        // preserve earlier good samples, detach the ISR and expose the error.
        ++current.rejectedReads;
        current.readFailures += (result.fault & 1) != 0;
        current.overlapReads += (result.fault & 2) != 0;
        current.readFault = result.fault;
        fault(3);
    }
    else if (current.running && millis() - lastReadMs >= 100)
    {
        // A silent input produces no interrupt to report its own failure.
        ++current.readyTimeouts;
        fault(2);
    }
    const uint32_t elapsedMs = millis() - rateWindowMs;

    // Report accepted reads per elapsed second, and publish status about every
    // 10 ms. The communications task independently controls UART transmission.
    if (current.running && elapsedMs >= 1000)
    {
        current.measuredRate = static_cast<uint32_t>((current.sampleCount - rateWindowCount) * 1000 / elapsedMs);
        rateWindowCount = current.sampleCount;
        rateWindowMs += elapsedMs;
    }
    if (millis() - lastPublishMs >= 10)
    {
        publish();
    }
}
