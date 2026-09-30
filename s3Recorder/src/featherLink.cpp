/**
 * @file featherLink.cpp
 * @brief Carry commands from Feather to S3 and send confirmed acquisition status back.
 *
 * main.cpp calls begin() once and update() repeatedly from Arduino's loop task.
 * Serial1 is the inter-board UART (asynchronous serial link); Serial is USB debug.
 * Incoming text becomes a validated command, then goes into acquisition's queue.
 * The acquisition task performs the hardware operation and publishes an answer.
 * This file sends that answer and periodic snapshots; it never reads ADC samples.
 *
 * "ack" means acknowledgement: a result paired with the command's request ID.
 * Accepting a command into a queue is not proof that the device performed it.
 */
#include "featherLink.h"
#include <cstdio>

/**
 * @brief Keep a reference to the acquisition service that outlives this link.
 * @param recorder The global acquisition object owned by main.cpp.
 *
 * Construction happens before Arduino setup, so no pins, tasks or serial ports
 * are started here. begin() performs hardware setup after the runtime is ready.
 */
featherLink::featherLink(acquisition& recorder) : recorder(recorder)
{
    // begin() opens the UART after the Arduino runtime is ready.
}

/**
 * @brief Open the dedicated Feather link at 115200 bits/second, receive 18/send 17.
 *
 * Serial framing is 8 data bits, no parity bit and 1 stop bit (SERIAL_8N1).
 * Pins 9-13 are already used by the digitizer. Receive/transmit buffers let the
 * main loop handle short bursts without waiting for each character on the wire.
 * The RX pull-up sets a disconnected input high, the serial line's idle level.
 */
void featherLink::begin()
{
    Serial1.setRxBufferSize(512);
    // Worst-case 64-bit counters can make a frame longer than the hardware FIFO.
    Serial1.setTxBufferSize(2048);
    // GPIO18 is UART1's native RX pad. pinMode() selects the ordinary GPIO
    // function, so it MUST run before begin() assigns the UART IOMUX function.
    // Calling it afterward disconnects UART reception despite correct wiring.
    pinMode(18, INPUT_PULLUP);
    Serial1.begin(115200, SERIAL_8N1, 18, 17);
}

/**
 * @brief Send a version-4 acquisition/recording report, including cumulative diagnostics.
 *
 * The first thirteen fields retain their previous meanings. Six added fields
 * carry detected misses, rejected reads, driver failures, overlapping reads,
 * ready timeouts, and the last read-fault flags. The appended recording fields come from the storage task snapshot.
 * If the UART has no room, skip this report; the next contains the same totals.
 */
bool featherLink::report(const acquisitionStatus& status)
{
    // Room for every counter at its full 64-bit decimal width plus all other fields.
    char frame[1536];
    // UINT32_MAX means "no sample yet"; otherwise age is time since the last
    // accepted sample, not the age of this periodic communications report.
    const uint32_t nowMs = millis();
    const uint32_t age = status.hasSample ? nowMs - status.lastSampleMs : UINT32_MAX;
#ifdef S3_LEGACY_STATUS
    // Bench builds can keep the currently deployed older Feather UI operational.
    // Only the UART frame changes; full diagnostics remain available over USB.
    int length = snprintf(frame, sizeof(frame),
        "S3,2,%lu,%u,%llu,%lu,%ld,%lu,%lu,%u,%lu,%lu,%lu\n",
        static_cast<unsigned long>(nowMs), status.ready ? 1 : 0,
        static_cast<unsigned long long>(status.sampleCount),
        static_cast<unsigned long>(status.running ? status.measuredRate : 0),
        static_cast<long>(status.latestRaw), static_cast<unsigned long>(age),
        static_cast<unsigned long>(status.error), status.running ? 1 : 0,
        static_cast<unsigned long>(status.rate), static_cast<unsigned long>(status.ackId),
        static_cast<unsigned long>(status.ackResult));
#else
    int length = snprintf(frame, sizeof(frame),
        "S3,4,%lu,%u,%llu,%lu,%ld,%lu,%lu,%u,%lu,%lu,%lu,%llu,%llu,%llu,%llu,%llu,%lu",
        static_cast<unsigned long>(nowMs), status.ready ? 1 : 0,
        static_cast<unsigned long long>(status.sampleCount),
        static_cast<unsigned long>(status.running ? status.measuredRate : 0),
        static_cast<long>(status.latestRaw), static_cast<unsigned long>(age),
        static_cast<unsigned long>(status.error), status.running ? 1 : 0,
        static_cast<unsigned long>(status.rate), static_cast<unsigned long>(status.ackId),
        static_cast<unsigned long>(status.ackResult),
        static_cast<unsigned long long>(status.missedEdges),
        static_cast<unsigned long long>(status.rejectedReads),
        static_cast<unsigned long long>(status.readFailures),
        static_cast<unsigned long long>(status.overlapReads),
        static_cast<unsigned long long>(status.readyTimeouts),
        static_cast<unsigned long>(status.readFault));
    // Append the shared numeric recording columns; no filenames or unescaped
    // text crosses this machine-readable link. Keep one complete newline frame.
    for (size_t index = 0; index < recordingStatus::fieldCount && length > 0 &&
        length < static_cast<int>(sizeof(frame)); ++index)
    {
        length += snprintf(frame + length, sizeof(frame) - length, ",%llu",
            static_cast<unsigned long long>(status.recording.values[index]));
    }
    if (length > 0 && length + 1 < static_cast<int>(sizeof(frame)))
    {
        frame[length++] = '\n';
        frame[length] = '\0';
    }
#endif
    // snprintf reports the length it needed. Reject truncation, and only write
    // when the whole line fits, so Feather never sees a deliberately partial report.
    const bool retVal = length > 0 && length < static_cast<int>(sizeof(frame)) &&
        Serial1.availableForWrite() >= length &&
        Serial1.write(reinterpret_cast<const uint8_t*>(frame), length) == static_cast<size_t>(length);
    return retVal;
}

/**
 * @brief Read pending commands, send status/replies, and finish a requested reboot.
 *
 * Called repeatedly by main.cpp on Arduino's loop task, not from an interrupt.
 * Read at most 128 bytes from each input per call so serial noise cannot keep us
 * here forever. Commands are queued without waiting; hardware work happens in
 * acquisition.cpp. Failed queue submission does not produce a success reply.
 *
 * Send two status reports per second and an earlier report for a new command
 * result. If transmission has no room, retry on a later call. For Reboot, wait
 * until its acknowledgement has left the UART, then restart after 100 ms.
 * USB diagnostics are separate human-readable messages, never protocol input.
 */
void featherLink::update()
{
    // Local bench console accepts the same strictly validated CMD,2 and CMD,3 frames.
    // Its separate parser prevents partial USB input mixing with Feather bytes.
    // This lets diagnostics start/stop collection without rewiring or changing
    // the DAC. Normal startup remains stopped; no command is generated here.
    for (size_t budget = 0; budget < 128 && Serial.available() && !rebootSent; ++budget)
    {
        acquisitionCommand command;
        if (usbParser.feed(static_cast<char>(Serial.read()), millis(), command))
        {
            recorder.submit(command);
        }
    }
    // A disconnected Feather does not stop acquisition. No link timeout changes
    // the recorder: S3 owns its run until Stop, a fault, or a reboot.
    for (size_t budget = 0; budget < 128 && Serial1.available() && !rebootSent; ++budget)
    {
        acquisitionCommand command;
        ++receivedBytes;
        if (parser.feed(static_cast<char>(Serial1.read()), millis(), command))
        {
            ++validCommands;
            // A full queue drops the request, never a previous accepted command.
            // Feather will explicitly time out instead of receiving false success.
            if (recorder.submit(command))
            {
                ++queuedCommands;
            }
        }
    }
    // Take one consistent copy: the other core may be collecting while we format
    // the message. No acquisition lock is held during formatting or transmission.
    const acquisitionStatus state = recorder.snapshot();
    const uint32_t nowMs = millis();
    // Compare both ID and result so a changed outcome is not hidden until the
    // periodic report. Update sent fields only after the UART accepted the frame.
    const bool newAck = state.ackId != sentAckId || state.ackResult != sentAckResult;
    if (!rebootSent && (nowMs - lastReportMs >= 500 || newAck))
    {
        if (report(state))
        {
            lastReportMs = nowMs;
            sentAckId = state.ackId;
            sentAckResult = state.ackResult;
            if (state.reboot && state.ackResult == 1)
            {
                // Flush only after acquisition stops. UART buffering is not proof
                // that the acknowledgement has left the pin before ESP.restart().
                Serial1.flush();
                rebootSent = true;
                rebootSentMs = millis();
            }
        }
    }
    if (rebootSent && millis() - rebootSentMs >= 100)
    {
        ESP.restart();
    }
    // USB diagnostics are once per second, not per conversion. Never mix them
    // into the machine-readable inter-board UART. Skip when USB TX is congested.
    if (nowMs - lastDebugMs >= 1000)
    {
        char text[200];
        int length = snprintf(text, sizeof(text),
            "ADC ready=%u running=%u target=%lu actual=%lu count=%llu raw=%ld error=%lu missedEdges=%llu readFault=%lu\n",
            state.ready ? 1 : 0, state.running ? 1 : 0, static_cast<unsigned long>(state.rate),
            static_cast<unsigned long>(state.measuredRate), static_cast<unsigned long long>(state.sampleCount),
            static_cast<long>(state.latestRaw), static_cast<unsigned long>(state.error),
            static_cast<unsigned long long>(state.missedEdges), static_cast<unsigned long>(state.readFault));
        if (length > 0 && length < static_cast<int>(sizeof(text)) && Serial.availableForWrite() >= length)
        {
            Serial.write(reinterpret_cast<const uint8_t*>(text), length);
        }
        // Distinguish an electrically silent RX wire from invalid protocol bytes
        // and from a command accepted by the worker. This goes only to USB debug.
        const int linkLength = snprintf(text, sizeof(text),
            "Link RX18=%d bytes=%lu valid=%lu queued=%lu ack=%lu/%lu readStage=%lu readUs=%lu\n",
            digitalRead(18), static_cast<unsigned long>(receivedBytes),
            static_cast<unsigned long>(validCommands), static_cast<unsigned long>(queuedCommands),
            static_cast<unsigned long>(state.ackId), static_cast<unsigned long>(state.ackResult),
            static_cast<unsigned long>(state.readDetail & 255), static_cast<unsigned long>(state.readDetail >> 8));
        if (linkLength > 0 && linkLength < static_cast<int>(sizeof(text)) && Serial.availableForWrite() >= linkLength)
        {
            Serial.write(reinterpret_cast<const uint8_t*>(text), linkLength);
        }
        // Timing is captured by acquisition, but formatted only here on core 1.
        // Unknown first-edge latency is printed as UINT32_MAX, never as zero.
        const int timingLength = snprintf(text, sizeof(text),
            "Timing wakeUs=%lu spiUs=%lu readUs=%lu maxWakeUs=%lu maxReadUs=%lu edges=%lu maxGapUs=%lu\n",
            static_cast<unsigned long>(state.wakeUs), static_cast<unsigned long>(state.spiUs),
            static_cast<unsigned long>(state.readDetail >> 8), static_cast<unsigned long>(state.maxWakeUs),
            static_cast<unsigned long>(state.maxReadUs), static_cast<unsigned long>(state.observedEdges),
            static_cast<unsigned long>(state.maxGapUs));
        if (timingLength > 0 && timingLength < static_cast<int>(sizeof(text)) && Serial.availableForWrite() >= timingLength)
        {
            Serial.write(reinterpret_cast<const uint8_t*>(text), timingLength);
        }
        const auto* recording = state.recording.values;
        const int storageLength = snprintf(text, sizeof(text),
            "SD card=%llu state=%llu file=%llu/%llu samples=%llu bytes=%llu ring=%llu/%llu peak=%llu maxUs=%llu lost=%llu errors=%llu\n",
            recording[1], recording[0], recording[3], recording[4], recording[7], recording[6],
            recording[11], recording[10], recording[12], recording[17], recording[21], recording[22]);
        if (storageLength > 0 && storageLength < static_cast<int>(sizeof(text)) && Serial.availableForWrite() >= storageLength)
        {
            Serial.write(reinterpret_cast<const uint8_t*>(text), storageLength);
        }
        lastDebugMs = nowMs;
    }
}
