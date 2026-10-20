/*
 * Copyright (C) 2026 Karl Stock
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of ESP32 Digitizer.
 * You may redistribute and/or modify it under the GNU General Public License
 * as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 *
 * Distributed WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License in the repository LICENSE file or <https://www.gnu.org/licenses/>.
 */

/**
 * @file ReceiverMonitor.cpp
 * @brief Feather-side serial receiver and command tracker for the receiver board.
 *
 * webApp calls begin() once and update() on each Arduino loop pass. update()
 * assembles serial lines; acceptLine() validates every field before storing it.
 * stateJson() supplies the browser snapshot. sendCommand() sends a request once
 * and leaves it pending until a matching receiver report confirms or rejects it.
 *
 * A fresh serial message proves only that receiver is communicating. To say samples
 * are arriving we also require its accepted-sample counter to advance recently.
 * A constant input voltage can produce identical values but advancing counts.
 * No full sample recording passes through this monitor.
 */
#include "ReceiverMonitor.h"
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
ReceiverMonitor::ReceiverMonitor(HardwareSerial& hardwareSerial) : _hardwareSerial(hardwareSerial)
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
void ReceiverMonitor::begin()
{
    // Buffer bursts while the same loop task is occupied serving an HTTP request.
    // RX receives status; TX sends commands. The peer must use the documented
    // receiver web-interface protocol and share ground with Feather.
    _hardwareSerial.setRxBufferSize(4096);
    // Set the idle pull-up before the UART owns the pin's peripheral function.
    pinMode(RX, INPUT_PULLUP);
    _hardwareSerial.begin(115200, SERIAL_8N1, RX, TX);
    // Randomize the ID sequence per boot to reduce the chance that an old receiver
    // reply matches a new request after restart. This is not a uniqueness
    // guarantee. Zero is reserved for 'no command'.
    _commandId = esp_random();
    Serial.println("receiver monitor: 115200 baud, Feather RX=GPIO7, TX=GPIO8; waiting for receiver.");
}

/**
 * @brief Parse strict unsigned decimal text without accepting signs or wraparound.
 * @param maximum Largest permitted field value.
 * @param value Written only when the entire field is valid.
 * @return True for a nonempty sequence of digits within range.
 */
bool ReceiverMonitor::readUnsigned(const char* text, uint64_t maximum, uint64_t& value)
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
 * @brief Validate a complete receiver status report before replacing the displayed state.
 *
 * Version 5 adds initial file-opening time and free-space measurement quality.
 * Version 4 has the original 24 recording fields; missing additions stay unknown.
 * Version 4 adds those fields to version 3's nineteen acquisition fields.
 * The whole report must validate before any part becomes visible. Older version-2
 * reports have thirteen fields and remain usable while boards are updated;
 * their missing diagnostics are reported as unknown, not as zero.
 * @param nowMs Feather receipt time; receiver uptime is never compared to the Feather clock.
 * @return True only after a whole frame has passed validation and been committed.
 *
 * Malformed/unsupported traffic cannot refresh 'connected'. Keeping temporary values
 * until all checks pass prevents a partial line from corrupting the displayed state.
 */
bool ReceiverMonitor::acceptLine(uint32_t nowMs)
{
    char* fields[45] = {_line};
    size_t fieldCount = 1;
    bool retVal = true;
    // Split in place, preserving empty fields so missing values are rejected.
    for (size_t index = 0; index < _lineLength; ++index)
    {
        if (_line[index] == ',')
        {
            _line[index] = '\0';
            if (fieldCount < 45)
            {
                fields[fieldCount++] = &_line[index + 1];
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
    retVal = retVal && strcmp(fields[0], "RECEIVER") == 0 &&
        (hasDiagnostics || (fieldCount == 13 && strcmp(fields[1], "2") == 0));
    uint64_t parsed[17] = {};
    RecordingStatus recordingStatusNext;
    // Validate recording fields into temporary storage. A corrupt extended field
    // must reject the whole frame, including its acquisition values and heartbeat.
    if (hasRecording)
    {
        const size_t columns = hasSpaceDetails ? RecordingStatus::_fieldCount : RecordingStatus::_legacyFieldCount;
        for (size_t index = 0; index < columns && retVal; ++index)
        {
            retVal = readUnsigned(fields[19 + index], UINT64_MAX, recordingStatusNext._values[index]);
        }
        retVal = retVal && recordingStatusNext.valid();
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
        const bool sameSession = _receivedFrame && nowMs - _lastFrameMs < _connectionTimeoutMs &&
            static_cast<uint32_t>(parsed[0] - _uptimeMs) < 0x80000000UL && parsed[2] >= _sampleCount;
        if (!sameSession || parsed[1] == 0)
        {
            _observedProgress = false;
        }
        else if (parsed[2] > _sampleCount)
        {
            _lastProgressMs = nowMs;
            _observedProgress = true;
        }
        // Commit the complete validated snapshot at once. parsed indexes follow
        // wire-field order after the receiver tag and version; keep them aligned with
        // shared/receiverStatusProtocol.md when extending the message.
        _uptimeMs = static_cast<uint32_t>(parsed[0]);
        _adcReady = parsed[1] != 0;
        _sampleCount = parsed[2];
        _diagnosticsAvailable = hasDiagnostics;
        _missedEdges = parsed[11];
        _rejectedReads = parsed[12];
        _readFailures = parsed[13];
        _overlapReads = parsed[14];
        _readyTimeouts = parsed[15];
        _readFault = static_cast<uint32_t>(parsed[16]);
        _samplesPerSecond = static_cast<uint32_t>(parsed[3]);
        _latestRaw = negative ? -static_cast<int32_t>(parsed[4]) : static_cast<int32_t>(parsed[4]);
        _sampleAgeMs = static_cast<uint32_t>(parsed[5]);
        _errorCode = static_cast<uint32_t>(parsed[6]);
        _acquisitionRunning = parsed[7] != 0;
        _appliedRate = static_cast<uint32_t>(parsed[8]);
        _recordingMonitor._available = hasRecording;
        _recordingMonitor._recordingStatus = recordingStatusNext;
        // A matching id alone is insufficient: a successful Start must also report
        // the requested running rate, and Stop/Reboot must report stopped acquisition.
        if (_commandStatus == "pending" && parsed[9] == _commandId && parsed[10] != 0)
        {
            const bool applied = _commandAction == "start" ?
                _acquisitionRunning && _appliedRate == _requestedRate &&
                (!_requestedRecording || (hasRecording && recordingStatusNext._values[RecordingStatus::state] == RecordingStatus::recording)) :
                !_acquisitionRunning && (!hasRecording ||
                (recordingStatusNext._values[RecordingStatus::state] != RecordingStatus::saving &&
                 recordingStatusNext._values[RecordingStatus::state] != RecordingStatus::deleting));
            _commandStatus = parsed[10] == 1 && applied ? "confirmed" : "rejected";
        }
        // Preparing/saving/deleting may take longer than a short command. A fresh
        // report bearing OUR pending request ID proves progress, not completion.
        // Keep waiting while that operation reports busy; loss of reports still
        // times out. An unrelated operation cannot extend this command's deadline.
        if (_commandStatus == "pending" && parsed[9] == _commandId && parsed[10] == 0 && hasRecording &&
            (recordingStatusNext._values[RecordingStatus::state] == RecordingStatus::preparing ||
             recordingStatusNext._values[RecordingStatus::state] == RecordingStatus::saving ||
             recordingStatusNext._values[RecordingStatus::state] == RecordingStatus::deleting))
        {
            _commandSentMs = nowMs;
        }
        _lastFrameMs = nowMs;
        _receivedFrame = true;
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
 * A command with no matching confirmation after five seconds becomes timeout,
 * unless fresh matching reports say preparation/saving/deletion is still busy;
 * those reports restart the five-second wait in acceptLine().
 * That means its outcome is unknown, not that receiver definitely did nothing. Do not
 * retry automatically: the next status can still show what receiver is actually doing.
 */
void ReceiverMonitor::update()
{
    const uint32_t nowMs = millis();
    // Missing acknowledgements become an explicit outcome; never automatically
    // retry Start/Reboot after an uncertain response. Polls still recover device state.
    if (_commandStatus == "pending" && nowMs - _commandSentMs >= 5000)
    {
        _commandStatus = "timeout";
    }
    // After expiry discard through newline; otherwise a later fragment could
    // accidentally be interpreted as the continuation of an unrelated report.
    if (_lineLength && nowMs - _lastByteMs > 500)
    {
        _lineLength = 0;
        _discardLine = true;
    }
    for (size_t budget = 0; budget < 256 && _hardwareSerial.available(); ++budget)
    {
        const char nextByte = static_cast<char>(_hardwareSerial.read());
        _lastByteMs = nowMs;
        // Newline is the only point where a report may replace displayed state.
        if (nextByte == '\n')
        {
            _line[_lineLength] = '\0';
            if (_discardLine || (_lineLength && !acceptLine(nowMs)))
            {
                ++_rejectedFrames;
            }
            _lineLength = 0;
            _discardLine = false;
        }
        else if (nextByte != '\r' && !_discardLine)
        {
            if (nextByte < ' ' || nextByte > '~' || _lineLength >= _lineCapacity - 1)
            {
                _discardLine = true;
            }
            else
            {
                _line[_lineLength++] = nextByte;
            }
        }
    }
}

/**
 * @brief Build a browser reply from the most recent validated receiver report.
 * @return JSON text with live measurements, or null measurements after disconnection.
 *
 * A report older than three seconds is disconnected. "receiving" additionally
 * requires running/ready flags, recent count progress, and a recently read sample.
 * receiver sample age is extended by time since Feather received the report; the boards'
 * absolute clocks are never compared. Stale values must not masquerade as live data.
 *
 * All 64-bit totals are decimal strings so JavaScript cannot round away digits.
 * Version-2 reports lack diagnostics and show them as null. Command outcome fields
 * remain available even when live measurements become unknown.
 */
String ReceiverMonitor::stateJson() const
{
    const uint32_t nowMs = millis();
    const uint32_t ageMs = nowMs - _lastFrameMs;
    const bool connected = _receivedFrame && ageMs < _connectionTimeoutMs;
    const bool receiving = connected && _acquisitionRunning && _adcReady && _observedProgress &&
        nowMs - _lastProgressMs < _connectionTimeoutMs &&
        static_cast<uint64_t>(_sampleAgeMs) + ageMs < _connectionTimeoutMs;
    String retVal = "{\"connected\":" + String(connected ? "true" : "false");
    retVal += ",\"lastMessageAgeMs\":" + (_receivedFrame ? String(ageMs) : String("null"));
    retVal += ",\"rejectedFrames\":" + String(_rejectedFrames);
    retVal += ",\"commandId\":" + String(_commandId);
    retVal += ",\"commandStatus\":\"" + _commandStatus + "\"";
    retVal += ",\"commandAction\":\"" + _commandAction + "\"";
    retVal += ",\"recording\":" + _recordingMonitor.stateJson(connected);
    // A counter is a decimal JSON string so browsers cannot round large totals.
    // Missing old-firmware diagnostics and disconnected values are explicitly null.
    const bool showDiagnostics = connected && _diagnosticsAvailable;
    retVal += ",\"diagnosticsAvailable\":" + String(showDiagnostics ? "true" : "false");
    const char* names[] = {"missedEdges", "rejectedReads", "readFailures", "overlapReads", "readyTimeouts"};
    const uint64_t totals[] = {_missedEdges, _rejectedReads, _readFailures, _overlapReads, _readyTimeouts};
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
    retVal += ",\"readFault\":" + (showDiagnostics ? String(_readFault) : String("null"));
    if (connected)
    {
        char countText[24];
        snprintf(countText, sizeof(countText), "%llu", static_cast<unsigned long long>(_sampleCount));
        retVal += ",\"adcReady\":" + String(_adcReady ? "true" : "false");
        retVal += ",\"receiving\":" + String(receiving ? "true" : "false");
        retVal += ",\"sampleCount\":\"" + String(countText) + "\"";
        retVal += ",\"samplesPerSecond\":" + String(_samplesPerSecond);
        retVal += ",\"latestRaw\":" + (_sampleCount ? String(_latestRaw) : String("null"));
        retVal += ",\"errorCode\":" + String(_errorCode);
        retVal += ",\"acquisitionRunning\":" + String(_acquisitionRunning ? "true" : "false");
        retVal += ",\"appliedRate\":" + String(_appliedRate);
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
 * This matches receiver's commandProtocol::rateRegister choices. Neither this test nor
 * the dropdown establishes that acquisition can sustain every offered rate.
 */
bool ReceiverMonitor::validRate(uint32_t rate)
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
 * @brief Send one validated command to receiver and mark it pending without waiting.
 * @param action start, stop, reboot, or delete; all other text is rejected.
 * @param rate Samples/second for Start; Stop/Reboot always transmit zero instead.
 * @param record Whether Start must also create a recording; false for other actions.
 * Deletion needs recording-capable firmware, a ready card and stopped acquisition.
 * @return True if the entire command was accepted by the serial transmit buffer.
 * False means disconnected, busy, invalid settings/state, or insufficient buffer room.
 *
 * Only the loop task calls this. A successful return is NOT hardware confirmation:
 * acceptLine() later checks the matching request ID, receiver result, and applied state.
 * The five-second timeout is handled in update(); there is no automatic retransmit.
 * An already running acquisition cannot be retuned to a different rate with Start.
 */
bool ReceiverMonitor::sendCommand(const char* action, uint32_t rate, bool record)
{
    const uint32_t nowMs = millis();
    const bool isStart = strcmp(action, "start") == 0;
    const bool isDelete = strcmp(action, "delete") == 0;
    const bool knownAction = isStart || isDelete || strcmp(action, "stop") == 0 || strcmp(action, "reboot") == 0;
    const auto recordingState = _recordingMonitor._recordingStatus._values[RecordingStatus::state];
    const bool storageBusy = _recordingMonitor._available && (recordingState == RecordingStatus::preparing ||
        recordingState == RecordingStatus::saving || recordingState == RecordingStatus::deleting);
    bool retVal = false;
    if (knownAction && _receivedFrame && nowMs - _lastFrameMs < _connectionTimeoutMs &&
        _commandStatus != "pending" && !storageBusy && (!record || (isStart && _recordingMonitor._available)) &&
        (!isDelete || (_recordingMonitor._available && !_acquisitionRunning &&
            _recordingMonitor._recordingStatus._values[RecordingStatus::card] == RecordingStatus::ready)) &&
        (!isStart || (_adcReady && validRate(rate) &&
        (!_acquisitionRunning || rate == _appliedRate))))
    {
        // Each request gets a new ID so an old reply cannot complete this one.
        // Skip zero after 32-bit wrap: zero means "no command" in the protocol.
        uint32_t nextId = _commandId + 1;
        if (nextId == 0)
        {
            nextId = 1;
        }
        // Older receivers keep receiving their original command format. Version 3 adds
        // the recording flag; version-4 or version-5 status advertises support.
        const String command = "CMD," + String(_recordingMonitor._available ? "3," : "2,") + String(nextId) + "," +
            action + "," + String(isStart ? rate : 0) +
            (_recordingMonitor._available ? String(record ? ",1" : ",0") : String("")) + "\n";
        // Require space for the whole line before writing. Remember pending state
        // only if every character was accepted, including the ending newline.
        if (_hardwareSerial.availableForWrite() >= static_cast<int>(command.length()))
        {
            retVal = _hardwareSerial.print(command) == command.length();
            if (retVal)
            {
                _commandId = nextId;
                _commandAction = action;
                _requestedRate = rate;
                _requestedRecording = record;
                _commandSentMs = nowMs;
                _commandStatus = "pending";
            }
        }
    }
    return retVal;
}
