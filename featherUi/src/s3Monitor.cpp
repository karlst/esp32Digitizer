/**
 * @file s3Monitor.cpp
 * @brief Feather-side serial receiver and command tracker for the S3 board.
 *
 * webApp calls begin() once and update() on each Arduino loop pass. update()
 * assembles serial lines; acceptLine() validates every field before storing it.
 * stateJson() supplies the browser snapshot. sendCommand() sends a request once
 * and leaves it pending until a matching S3 report confirms or rejects it.
 *
 * A fresh serial message proves only that S3 is communicating. To say samples
 * are arriving we also require its accepted-sample counter to advance recently.
 * A constant input voltage can produce identical values but advancing counts.
 * No full sample recording passes through this monitor.
 */
#include "s3Monitor.h"
#include <cstring>
#include <cstdio>
#include <esp_system.h>

/**
 * @brief Keep a reference to the dedicated inter-board serial port.
 * @param serialPort Serial1 owned by the runtime; USB diagnostics use Serial instead.
 *
 * The constructor can run before Arduino startup, so hardware work waits for begin().
 * This object is used only by the loop task, not the DAC worker or an interrupt.
 */
s3Monitor::s3Monitor(HardwareSerial& serialPort) : serialPort(serialPort)
{
    // Configuration waits until Arduino setup so its runtime is initialized.
}

/**
 * @brief Start the header UART independently of USB programming and debug output.
 *
 * The Feather V2 board definition maps RX to GPIO7 and TX to GPIO8. Named pins
 * deliberately avoid the original Feather's incompatible GPIO16/17 assignments.
 * RX is pulled up so a missing cable rests at the UART's idle-high level.
 */
void s3Monitor::begin()
{
    // Buffer bursts while the same loop task is occupied serving an HTTP request.
    // RX receives S3 pin 17; TX sends commands to S3 pin 18, with common ground.
    serialPort.setRxBufferSize(4096);
    // Set the idle pull-up before the UART owns the pin's peripheral function.
    pinMode(RX, INPUT_PULLUP);
    serialPort.begin(115200, SERIAL_8N1, RX, TX);
    // Randomize the id sequence per Feather boot so old S3 replies cannot satisfy
    // a new command after Feather restarts. Zero is reserved for 'no command'.
    commandId = esp_random();
    Serial.println("S3 monitor: 115200 baud, Feather RX=GPIO7, TX=GPIO8; waiting for S3.");
}

/**
 * @brief Parse strict unsigned decimal text without accepting signs or wraparound.
 * @param maximum Largest permitted field value.
 * @param value Written only when the entire field is valid.
 * @return True for a nonempty sequence of digits within range.
 */
bool s3Monitor::readUnsigned(const char* text, uint64_t maximum, uint64_t& value)
{
    bool retVal = *text != '\0';
    uint64_t parsed = 0;
    // Check before multiplication; even a long string cannot overflow the accumulator.
    for (size_t index = 0; text[index] != '\0' && retVal; ++index)
    {
        const char digit = text[index];
        if (digit < '0' || digit > '9' || static_cast<uint64_t>(digit - '0') > maximum ||
            parsed > (maximum - (digit - '0')) / 10)
        {
            retVal = false;
        }
        else
        {
            parsed = parsed * 10 + (digit - '0');
        }
    }
    if (retVal && parsed <= maximum)
    {
        value = parsed;
    }
    else
    {
        retVal = false;
    }
    return retVal;
}

/**
 * @brief Validate a complete S3 status report before replacing the displayed state.
 *
 * Version 5 adds initial file-opening time and free-space measurement quality.
 * Version 4 has the original 24 recording fields; missing additions stay unknown.
 * Version 4 adds those fields to version 3's nineteen acquisition fields.
 * The whole report must validate before any part becomes visible. Older version-2
 * reports have thirteen fields and remain usable while boards are updated;
 * their missing diagnostics are reported as unknown, not as zero.
 * @param nowMs Feather receipt time; S3 uptime is never compared to the Feather clock.
 * @return True only after a whole frame has passed validation and been committed.
 *
 * Malformed/unsupported traffic cannot refresh 'connected'. Keeping temporary values
 * until all checks pass prevents a partial line from corrupting the displayed state.
 */
bool s3Monitor::acceptLine(uint32_t nowMs)
{
    char* fields[45] = {line};
    size_t fieldCount = 1;
    bool retVal = true;
    // Split in place, preserving empty fields so missing values are rejected.
    for (size_t index = 0; index < lineLength; ++index)
    {
        if (line[index] == ',')
        {
            line[index] = '\0';
            if (fieldCount < 45)
            {
                fields[fieldCount++] = &line[index + 1];
            }
            else
            {
                retVal = false;
            }
        }
    }
    const bool hasSpaceDetails = fieldCount == 45 && strcmp(fields[1], "5") == 0;
    const bool hasRecording = hasSpaceDetails || (fieldCount == 43 && strcmp(fields[1], "4") == 0);
    const bool hasDiagnostics = hasRecording || (fieldCount == 19 && strcmp(fields[1], "3") == 0);
    retVal = retVal && strcmp(fields[0], "S3") == 0 &&
        (hasDiagnostics || (fieldCount == 13 && strcmp(fields[1], "2") == 0));
    uint64_t parsed[17] = {};
    recordingStatus nextRecording;
    // Validate recording fields into temporary storage. A corrupt extended field
    // must reject the whole frame, including its acquisition values and heartbeat.
    if (hasRecording)
    {
        const size_t columns = hasSpaceDetails ? recordingStatus::fieldCount : recordingStatus::legacyFieldCount;
        for (size_t index = 0; index < columns && retVal; ++index)
        {
            retVal = readUnsigned(fields[19 + index], UINT64_MAX, nextRecording.values[index]);
        }
        retVal = retVal && nextRecording.valid();
    }
    bool negative = false;
    if (retVal)
    {
        // Counts can exceed 32 bits during long runs. Raw ADC data is signed 24-bit.
        negative = fields[6][0] == '-';
        const char* rawDigits = fields[6] + (negative ? 1 : 0);
        retVal = readUnsigned(fields[2], UINT32_MAX, parsed[0]) &&
            readUnsigned(fields[3], 1, parsed[1]) &&
            readUnsigned(fields[4], UINT64_MAX, parsed[2]) &&
            readUnsigned(fields[5], UINT32_MAX, parsed[3]) &&
            readUnsigned(rawDigits, negative ? 8388608 : 8388607, parsed[4]) &&
            readUnsigned(fields[7], UINT32_MAX, parsed[5]) &&
            readUnsigned(fields[8], UINT32_MAX, parsed[6]) &&
            readUnsigned(fields[9], 1, parsed[7]) &&
            readUnsigned(fields[10], 30000, parsed[8]) && validRate(static_cast<uint32_t>(parsed[8])) &&
            readUnsigned(fields[11], UINT32_MAX, parsed[9]) &&
            readUnsigned(fields[12], 2, parsed[10]);
        // Validate every added field before committing any of this report. Keep
        // 64-bit counts intact; last-fault flags have only two defined bits.
        if (retVal && hasDiagnostics)
        {
            retVal = readUnsigned(fields[13], UINT64_MAX, parsed[11]) &&
                readUnsigned(fields[14], UINT64_MAX, parsed[12]) &&
                readUnsigned(fields[15], UINT64_MAX, parsed[13]) &&
                readUnsigned(fields[16], UINT64_MAX, parsed[14]) &&
                readUnsigned(fields[17], UINT64_MAX, parsed[15]) &&
                readUnsigned(fields[18], 3, parsed[16]);
        }
    }
    if (retVal)
    {
        // A fresh heartbeat is not evidence of acquisition. Require counter progress
        // across two frames within the current connection before reporting samples.
        // Counter reset or a backward uptime jump starts observation again; unsigned
        // uptime subtraction handles the normal millis() wrap without a false restart.
        const bool sameSession = receivedFrame && nowMs - lastFrameMs < connectionTimeoutMs &&
            static_cast<uint32_t>(parsed[0] - uptimeMs) < 0x80000000UL && parsed[2] >= sampleCount;
        if (!sameSession || parsed[1] == 0)
        {
            observedProgress = false;
        }
        else if (parsed[2] > sampleCount)
        {
            lastProgressMs = nowMs;
            observedProgress = true;
        }
        // Commit the complete validated snapshot at once. parsed indexes follow
        // wire-field order after the S3 tag and version; keep them aligned with
        // shared/s3StatusProtocol.md when extending the message.
        uptimeMs = static_cast<uint32_t>(parsed[0]);
        adcReady = parsed[1] != 0;
        sampleCount = parsed[2];
        diagnosticsAvailable = hasDiagnostics;
        missedEdges = parsed[11];
        rejectedReads = parsed[12];
        readFailures = parsed[13];
        overlapReads = parsed[14];
        readyTimeouts = parsed[15];
        readFault = static_cast<uint32_t>(parsed[16]);
        samplesPerSecond = static_cast<uint32_t>(parsed[3]);
        latestRaw = negative ? -static_cast<int32_t>(parsed[4]) : static_cast<int32_t>(parsed[4]);
        sampleAgeMs = static_cast<uint32_t>(parsed[5]);
        errorCode = static_cast<uint32_t>(parsed[6]);
        acquisitionRunning = parsed[7] != 0;
        appliedRate = static_cast<uint32_t>(parsed[8]);
        recording.available = hasRecording;
        recording.status = nextRecording;
        // A matching id alone is insufficient: a successful Start must also report
        // the requested running rate, and Stop/Reboot must report stopped acquisition.
        if (commandStatus == "pending" && parsed[9] == commandId && parsed[10] != 0)
        {
            const bool applied = commandAction == "start" ?
                acquisitionRunning && appliedRate == requestedRate &&
                (!requestedRecording || (hasRecording && nextRecording.values[recordingStatus::state] == recordingStatus::recording)) :
                !acquisitionRunning && (!hasRecording ||
                (nextRecording.values[recordingStatus::state] != recordingStatus::saving &&
                 nextRecording.values[recordingStatus::state] != recordingStatus::deleting));
            commandStatus = parsed[10] == 1 && applied ? "confirmed" : "rejected";
        }
        // Preparing/saving/deleting may take longer than a short command. A fresh
        // report bearing OUR pending request ID proves progress, not completion.
        // Keep waiting while that operation reports busy; loss of reports still
        // times out. An unrelated operation cannot extend this command's deadline.
        if (commandStatus == "pending" && parsed[9] == commandId && parsed[10] == 0 && hasRecording &&
            (nextRecording.values[recordingStatus::state] == recordingStatus::preparing ||
             nextRecording.values[recordingStatus::state] == recordingStatus::saving ||
             nextRecording.values[recordingStatus::state] == recordingStatus::deleting))
        {
            commandSentMs = nowMs;
        }
        lastFrameMs = nowMs;
        receivedFrame = true;
    }
    return retVal;
}

/**
 * @brief Drain available serial bytes and update pending-command timeout state.
 *
 * Called from webApp, never from an interrupt. Read at most 256 bytes per call;
 * longer buffered input is handled by the next pass. No call waits for another byte.
 * A whole valid line refreshes the link; malformed text does not. Lines are capped
 * by lineCapacity and fragments expire after 500 ms. LF ends a line; CR is ignored.
 *
 * A command with no matching confirmation after five seconds becomes timeout.
 * That means its outcome is unknown, not that S3 definitely did nothing. Do not
 * retry automatically: the next status can still show what S3 is actually doing.
 */
void s3Monitor::update()
{
    const uint32_t nowMs = millis();
    // Missing acknowledgements become an explicit outcome; never automatically
    // retry Start/Reboot after an uncertain response. Polls still recover device state.
    if (commandStatus == "pending" && nowMs - commandSentMs >= 5000)
    {
        commandStatus = "timeout";
    }
    // After expiry discard through newline; otherwise a later fragment could
    // accidentally be interpreted as the continuation of an unrelated report.
    if (lineLength && nowMs - lastByteMs > 500)
    {
        lineLength = 0;
        discardLine = true;
    }
    for (size_t budget = 0; budget < 256 && serialPort.available(); ++budget)
    {
        const char nextByte = static_cast<char>(serialPort.read());
        lastByteMs = nowMs;
        // Newline is the only point where a report may replace displayed state.
        if (nextByte == '\n')
        {
            line[lineLength] = '\0';
            if (discardLine || (lineLength && !acceptLine(nowMs)))
            {
                ++rejectedFrames;
            }
            lineLength = 0;
            discardLine = false;
        }
        else if (nextByte != '\r' && !discardLine)
        {
            if (nextByte < ' ' || nextByte > '~' || lineLength >= lineCapacity - 1)
            {
                discardLine = true;
            }
            else
            {
                line[lineLength++] = nextByte;
            }
        }
    }
}

/**
 * @brief Build a browser reply from the most recent validated S3 report.
 * @return JSON text with live measurements, or null measurements after disconnection.
 *
 * A report older than three seconds is disconnected. "receiving" additionally
 * requires running/ready flags, recent count progress, and a recently read sample.
 * S3 sample age is extended by time since Feather received the report; the boards'
 * absolute clocks are never compared. Stale values must not masquerade as live data.
 *
 * All 64-bit totals are decimal strings so JavaScript cannot round away digits.
 * Version-2 reports lack diagnostics and show them as null. Command outcome fields
 * remain available even when live measurements become unknown.
 */
String s3Monitor::stateJson() const
{
    const uint32_t nowMs = millis();
    const uint32_t ageMs = nowMs - lastFrameMs;
    const bool connected = receivedFrame && ageMs < connectionTimeoutMs;
    const bool receiving = connected && acquisitionRunning && adcReady && observedProgress &&
        nowMs - lastProgressMs < connectionTimeoutMs &&
        static_cast<uint64_t>(sampleAgeMs) + ageMs < connectionTimeoutMs;
    String retVal = "{\"connected\":" + String(connected ? "true" : "false");
    retVal += ",\"lastMessageAgeMs\":" + (receivedFrame ? String(ageMs) : String("null"));
    retVal += ",\"rejectedFrames\":" + String(rejectedFrames);
    retVal += ",\"commandId\":" + String(commandId);
    retVal += ",\"commandStatus\":\"" + commandStatus + "\"";
    retVal += ",\"commandAction\":\"" + commandAction + "\"";
    retVal += ",\"recording\":" + recording.stateJson(connected);
    // A counter is a decimal JSON string so browsers cannot round large totals.
    // Missing old-firmware diagnostics and disconnected values are explicitly null.
    const bool showDiagnostics = connected && diagnosticsAvailable;
    retVal += ",\"diagnosticsAvailable\":" + String(showDiagnostics ? "true" : "false");
    const char* names[] = {"missedEdges", "rejectedReads", "readFailures", "overlapReads", "readyTimeouts"};
    const uint64_t totals[] = {missedEdges, rejectedReads, readFailures, overlapReads, readyTimeouts};
    for (size_t index = 0; index < 5; ++index)
    {
        retVal += ",\"" + String(names[index]) + "\":";
        if (showDiagnostics)
        {
            char totalText[24];
            snprintf(totalText, sizeof(totalText), "%llu", static_cast<unsigned long long>(totals[index]));
            retVal += "\"" + String(totalText) + "\"";
        }
        else
        {
            retVal += "null";
        }
    }
    retVal += ",\"readFault\":" + (showDiagnostics ? String(readFault) : String("null"));
    if (connected)
    {
        char countText[24];
        snprintf(countText, sizeof(countText), "%llu", static_cast<unsigned long long>(sampleCount));
        retVal += ",\"adcReady\":" + String(adcReady ? "true" : "false");
        retVal += ",\"receiving\":" + String(receiving ? "true" : "false");
        retVal += ",\"sampleCount\":\"" + String(countText) + "\"";
        retVal += ",\"samplesPerSecond\":" + String(samplesPerSecond);
        retVal += ",\"latestRaw\":" + (sampleCount ? String(latestRaw) : String("null"));
        retVal += ",\"errorCode\":" + String(errorCode);
        retVal += ",\"acquisitionRunning\":" + String(acquisitionRunning ? "true" : "false");
        retVal += ",\"appliedRate\":" + String(appliedRate);
    }
    else
    {
        retVal += ",\"adcReady\":null,\"receiving\":null,\"sampleCount\":null";
        retVal += ",\"samplesPerSecond\":null,\"latestRaw\":null,\"errorCode\":null";
        retVal += ",\"acquisitionRunning\":null,\"appliedRate\":null";
    }
    retVal += '}';
    return retVal;
}

/**
 * @brief Check that samples-per-second is one of the seven rates offered by this UI.
 * @return True for an exact supported rate, not for every integer below 30000.
 *
 * This matches S3's commandProtocol::rateRegister choices. Neither this test nor
 * the dropdown establishes that acquisition can sustain every offered rate.
 */
bool s3Monitor::validRate(uint32_t rate)
{
    const uint32_t supported[] = {100, 500, 1000, 2000, 7500, 15000, 30000};
    bool retVal = false;
    for (const uint32_t candidate : supported)
    {
        retVal = retVal || rate == candidate;
    }
    return retVal;
}

/**
 * @brief Send one validated command to S3 and mark it pending without waiting.
 * @param action start, stop, reboot, or delete; all other text is rejected.
 * @param rate Samples/second for Start; Stop/Reboot always transmit zero instead.
 * @param record Whether Start must also create a recording; false for other actions.
 * Deletion needs recording-capable firmware, a ready card and stopped acquisition.
 * @return True if the entire command was accepted by the serial transmit buffer.
 * False means disconnected, busy, invalid settings/state, or insufficient buffer room.
 *
 * Only the loop task calls this. A successful return is NOT hardware confirmation:
 * acceptLine() later checks the matching request ID, S3 result, and applied state.
 * The five-second timeout is handled in update(); there is no automatic retransmit.
 * An already running acquisition cannot be retuned to a different rate with Start.
 */
bool s3Monitor::sendCommand(const char* action, uint32_t rate, bool record)
{
    const uint32_t nowMs = millis();
    const bool isStart = strcmp(action, "start") == 0;
    const bool isDelete = strcmp(action, "delete") == 0;
    const bool knownAction = isStart || isDelete || strcmp(action, "stop") == 0 || strcmp(action, "reboot") == 0;
    const auto recordingState = recording.status.values[recordingStatus::state];
    const bool storageBusy = recording.available && (recordingState == recordingStatus::preparing ||
        recordingState == recordingStatus::saving || recordingState == recordingStatus::deleting);
    bool retVal = false;
    if (knownAction && receivedFrame && nowMs - lastFrameMs < connectionTimeoutMs &&
        commandStatus != "pending" && !storageBusy && (!record || (isStart && recording.available)) &&
        (!isDelete || (recording.available && !acquisitionRunning &&
            recording.status.values[recordingStatus::card] == recordingStatus::ready)) &&
        (!isStart || (adcReady && validRate(rate) &&
        (!acquisitionRunning || rate == appliedRate))))
    {
        // Each request gets a new ID so an old reply cannot complete this one.
        // Skip zero after 32-bit wrap: zero means "no command" in the protocol.
        uint32_t nextId = commandId + 1;
        if (nextId == 0)
        {
            nextId = 1;
        }
        // Older S3s keep receiving their original command format. Version 3 adds
        // the recording flag; only a version-4 status advertises support for it.
        const String command = "CMD," + String(recording.available ? "3," : "2,") + String(nextId) + "," +
            action + "," + String(isStart ? rate : 0) +
            (recording.available ? String(record ? ",1" : ",0") : String("")) + "\n";
        // Require space for the whole line before writing. Remember pending state
        // only if every character was accepted, including the ending newline.
        if (serialPort.availableForWrite() >= static_cast<int>(command.length()))
        {
            retVal = serialPort.print(command) == command.length();
            if (retVal)
            {
                commandId = nextId;
                commandAction = action;
                requestedRate = rate;
                requestedRecording = record;
                commandSentMs = nowMs;
                commandStatus = "pending";
            }
        }
    }
    return retVal;
}
