/**
 * @file featherLink.cpp
 * @brief Bounded command input and confirmed-state output for the existing Feather UI.
 */
#include "featherLink.h"
#include <cstdio>

/**
 * @brief Bind the recorder without starting hardware during global initialization.
 */
featherLink::featherLink(acquisition& recorder) : recorder(recorder)
{
    // begin() opens the UART after the Arduino runtime is ready.
}

/**
 * @brief Assign RX18/TX17 explicitly; digitizer SPI remains on GPIO9 through GPIO13.
 */
void featherLink::begin()
{
    Serial1.setRxBufferSize(512);
    // Worst-case 64-bit counters can make a frame longer than the hardware FIFO.
    Serial1.setTxBufferSize(512);
    // GPIO18 is UART1's native RX pad. pinMode() selects the ordinary GPIO
    // function, so it MUST run before begin() assigns the UART IOMUX function.
    // Calling it afterward disconnects UART reception despite correct wiring.
    pinMode(18, INPUT_PULLUP);
    Serial1.begin(115200, SERIAL_8N1, 18, 17);
}

/**
 * @brief Send exactly thirteen fields from one consistent snapshot, without blocking.
 */
bool featherLink::report(const acquisitionStatus& status)
{
    char frame[160];
    const uint32_t nowMs = millis();
    const uint32_t age = status.hasSample ? nowMs - status.lastSampleMs : UINT32_MAX;
    const int length = snprintf(frame, sizeof(frame),
        "S3,2,%lu,%u,%llu,%lu,%ld,%lu,%lu,%u,%lu,%lu,%lu\n",
        static_cast<unsigned long>(nowMs), status.ready ? 1 : 0,
        static_cast<unsigned long long>(status.sampleCount),
        static_cast<unsigned long>(status.running ? status.measuredRate : 0),
        static_cast<long>(status.latestRaw), static_cast<unsigned long>(age),
        static_cast<unsigned long>(status.error), status.running ? 1 : 0,
        static_cast<unsigned long>(status.rate), static_cast<unsigned long>(status.ackId),
        static_cast<unsigned long>(status.ackResult));
    const bool retVal = length > 0 && length < static_cast<int>(sizeof(frame)) &&
        Serial1.availableForWrite() >= length &&
        Serial1.write(reinterpret_cast<const uint8_t*>(frame), length) == static_cast<size_t>(length);
    return retVal;
}

/**
 * @brief Drain bounded input, send periodic/immediate acknowledgements, and finish reboot.
 */
void featherLink::update()
{
    // Local bench console accepts the same strictly validated CMD,2 frames.
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
    const acquisitionStatus state = recorder.snapshot();
    const uint32_t nowMs = millis();
    const bool newAck = state.ackId != sentAckId || state.ackResult != sentAckResult;
    if (!rebootSent && (nowMs - lastReportMs >= 1000 || newAck))
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
        const int length = snprintf(text, sizeof(text),
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
        lastDebugMs = nowMs;
    }
}
