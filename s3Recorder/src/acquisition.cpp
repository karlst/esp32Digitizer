/**
 * @file acquisition.cpp
 * @brief Interrupt-woken ADC worker; the interrupt itself never performs SPI reads.
 */
#include "acquisition.h"

/**
 * @brief Allocate the one-command mailbox and start the ADC owner on core zero.
 */
bool acquisition::begin()
{
    commands = xQueueCreate(1, sizeof(acquisitionCommand));
    const bool retVal = commands && xTaskCreatePinnedToCore(taskEntry, "adcReader", 4096,
        this, 3, &worker, 0) == pdPASS;
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
 * @brief Enqueue without blocking the UART loop; full mailboxes are never overwritten.
 */
bool acquisition::submit(const acquisitionCommand& command)
{
    const bool retVal = commands && xQueueSend(commands, &command, 0) == pdTRUE;
    return retVal;
}

/**
 * @brief Copy a coherent record; neither caller nor worker holds the lock afterward.
 */
acquisitionStatus acquisition::snapshot()
{
    portENTER_CRITICAL(&snapshotLock);
    const acquisitionStatus retVal = published;
    portEXIT_CRITICAL(&snapshotLock);
    return retVal;
}

/**
 * @brief Publish about every 10 ms or immediately after commands/faults, not per sample.
 */
void acquisition::publish()
{
    portENTER_CRITICAL(&snapshotLock);
    published = current;
    portEXIT_CRITICAL(&snapshotLock);
    lastPublishMs = millis();
}

/**
 * @brief Bridge the FreeRTOS C entry point to this object's permanent worker loop.
 */
void acquisition::taskEntry(void* argument)
{
    static_cast<acquisition*>(argument)->run();
}

/**
 * @brief Count readiness edges and wake the worker, with no conversion or logging in ISR.
 */
void IRAM_ATTR acquisition::readyInterrupt(void* argument)
{
    acquisition* self = static_cast<acquisition*>(argument);
    ++self->readyEdges;
    BaseType_t wake = pdFALSE;
    vTaskNotifyGiveFromISR(self->worker, &wake);
    if (wake == pdTRUE)
    {
        portYIELD_FROM_ISR();
    }
}

/**
 * @brief Stop software acquisition after lost DRDY or an ambiguous read; retain evidence.
 */
void acquisition::fault(uint32_t error)
{
    detachInterrupt(ads1256::readyPin);
    adc.stop();
    current.running = false;
    current.ready = false;
    current.measuredRate = 0;
    current.error = error;
    publish();
}

/**
 * @brief Apply a queued request, acknowledging only after hardware work completes.
 */
void acquisition::execute(const acquisitionCommand& command)
{
    // The Feather allows one outstanding command and never automatically retries.
    // Retain recent results so a delayed Start after Stop cannot restart a run.
    uint32_t savedResult = 0;
    if (history.lookup(command, savedResult))
    {
        current.ackId = command.id;
        current.ackResult = savedResult;
    }
    else
    {
        bool accepted = false;
        if (command.action == acquisitionCommand::Action::start)
        {
            if (current.running)
            {
                accepted = current.rate == command.rate;
            }
            else if (current.ready)
            {
                accepted = adc.start(command.rate);
                if (accepted)
                {
                    current.rate = command.rate;
                    current.running = true;
                    current.error = 0;
                    current.readFault = 0;
                    current.readDetail = 0;
                    current.measuredRate = 0;
                    // Discard stale notification counts from the previous run.
                    ulTaskNotifyTake(pdTRUE, 0);
                    readyEdges = 0;
                    previousEdge = 0;
                    attachInterruptArg(ads1256::readyPin, readyInterrupt, this, FALLING);
                    // DRDY can already be low on attachment. The worker tests its
                    // level as well as notifications, so it won't lose the first result.
                    lastReadMs = millis();
                    rateWindowMs = lastReadMs;
                    rateWindowCount = current.sampleCount;
                }
                else
                {
                    current.ready = false;
                    current.error = 3;
                }
            }
        }
        else
        {
            detachInterrupt(ads1256::readyPin);
            const bool stopped = adc.stop();
            current.running = false;
            current.measuredRate = 0;
            if (!stopped)
            {
                current.ready = false;
                current.error = 2;
            }
            // Stop means the S3 no longer reads samples, even if hardware has
            // failed. Keep ADC faults visible; reboot remains a recovery action.
            accepted = true;
            current.reboot = command.action == acquisitionCommand::Action::reboot;
        }
        current.ackId = command.id;
        current.ackResult = accepted ? 1 : 2;
        history.remember(command, current.ackResult);
    }
    publish();
}

/**
 * @brief Service the ADC independently of USB/UART; count only complete fresh reads.
 */
void acquisition::run()
{
    current.ready = adc.begin();
    current.error = current.ready ? 0 : 1;
    publish();
    for (;;)
    {
        acquisitionCommand command;
        if (!current.reboot && xQueueReceive(commands, &command, 0) == pdTRUE)
        {
            execute(command);
        }
        if (current.running)
        {
            collect();
        }
        else
        {
            // Stopped acquisition consumes no SPI bandwidth and yields its core.
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

/**
 * @brief Handle one readiness event and update measured throughput; called only by the worker.
 */
void acquisition::collect()
{
    if (current.running)
    {
        if (digitalRead(ads1256::readyPin) != LOW)
        {
            // Sleep until data is ready; a bounded wait also services Stop
            // and detects an unplugged/stuck-high DRDY wire within 100 ms.
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
        }
        if (digitalRead(ads1256::readyPin) == LOW)
        {
            const uint32_t edgeBefore = readyEdges;
            const uint32_t elapsedEdges = edgeBefore - previousEdge;
            int32_t sample = 0;
            const bool complete = adc.read(sample);
            // A new result arriving during a read can corrupt its bytes.
            // Stop on ambiguity instead of counting it as a good conversion.
            if (complete && readyEdges == edgeBefore)
            {
                if (elapsedEdges > 1)
                {
                    current.missedEdges += elapsedEdges - 1;
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
                current.readFault = (complete ? 0U : 1U) | (readyEdges == edgeBefore ? 0U : 2U);
                current.readDetail = adc.readDiagnostic();
                fault(3);
            }
        }
        else if (millis() - lastReadMs >= 100)
        {
            fault(2);
        }
        // Measure successful reads over actual elapsed time. Requested rate
        // is never substituted for measured throughput or conversion count.
        const uint32_t elapsedMs = millis() - rateWindowMs;
        if (current.running && elapsedMs >= 1000)
        {
            current.measuredRate = static_cast<uint32_t>(
                (current.sampleCount - rateWindowCount) * 1000 / elapsedMs);
            rateWindowCount = current.sampleCount;
            rateWindowMs += elapsedMs;
        }
        if (millis() - lastPublishMs >= 10)
        {
            publish();
        }
    }
}
