/**
 * @file s3Monitor.cpp
 * @brief Separate S3 link health from evidence that ADC conversions are arriving.
 */
#include "s3Monitor.h"
#include <cstring>
#include <cstdio>
#include <esp_system.h>

/**
 * @brief Bind the UART without touching hardware during global construction.
 * @param serialPort Dedicated inter-board UART; do not use the USB debug Serial.
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
    serialPort.setRxBufferSize(1024);
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
 * @brief Accept thirteen version-2 fields, including applied state and command acknowledgement.
 * @param nowMs Feather receipt time; S3 uptime is never compared to the Feather clock.
 * @return True only after a whole frame has passed validation and been committed.
 *
 * Malformed/unsupported traffic cannot refresh 'connected'. Keeping temporary values
 * until all checks pass prevents a partial line from corrupting the displayed state.
 */
bool s3Monitor::acceptLine(uint32_t nowMs)
{
    char* fields[13] = {line};
    size_t fieldCount = 1;
    bool retVal = true;
    // Split in place, preserving empty fields so missing values are rejected.
    for (size_t index = 0; index < lineLength; ++index)
    {
        if (line[index] == ',')
        {
            line[index] = '\0';
            if (fieldCount < 13)
            {
                fields[fieldCount++] = &line[index + 1];
            }
            else
            {
                retVal = false;
            }
        }
    }
    retVal = retVal && fieldCount == 13 && strcmp(fields[0], "S3") == 0 && strcmp(fields[1], "2") == 0;
    uint64_t parsed[11] = {};
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
        uptimeMs = static_cast<uint32_t>(parsed[0]);
        adcReady = parsed[1] != 0;
        sampleCount = parsed[2];
        samplesPerSecond = static_cast<uint32_t>(parsed[3]);
        latestRaw = negative ? -static_cast<int32_t>(parsed[4]) : static_cast<int32_t>(parsed[4]);
        sampleAgeMs = static_cast<uint32_t>(parsed[5]);
        errorCode = static_cast<uint32_t>(parsed[6]);
        acquisitionRunning = parsed[7] != 0;
        appliedRate = static_cast<uint32_t>(parsed[8]);
        // A matching id alone is insufficient: a successful Start must also report
        // the requested running rate, and Stop/Reboot must report stopped acquisition.
        if (commandStatus == "pending" && parsed[9] == commandId && parsed[10] != 0)
        {
            const bool applied = commandAction == "start" ?
                acquisitionRunning && appliedRate == requestedRate : !acquisitionRunning;
            commandStatus = parsed[10] == 1 && applied ? "confirmed" : "rejected";
        }
        lastFrameMs = nowMs;
        receivedFrame = true;
    }
    return retVal;
}

/**
 * @brief Consume at most 256 buffered bytes per pass without waiting for a newline.
 *
 * At one short report per second this is ample capacity. A bounded loop keeps
 * arbitrary UART noise from starving the browser. CRLF and LF are both accepted;
 * other control bytes, oversize lines, and expired fragments are discarded.
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
    if (lineLength && nowMs - lastByteMs > 500)
    {
        lineLength = 0;
        discardLine = true;
    }
    for (size_t budget = 0; budget < 256 && serialPort.available(); ++budget)
    {
        const char nextByte = static_cast<char>(serialPort.read());
        lastByteMs = nowMs;
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
 * @brief Serialize fresh state, using null for measurements after a link timeout.
 *
 * Link age uses only Feather time; sample age is the S3's reported age plus time
 * elapsed since receipt. A disconnected or stalled device must not look live.
 * sampleCount is a decimal JSON string to preserve integers above JavaScript's
 * exact numeric range. A steady voltage is valid data; raw value changes are not
 * required to establish reception, only an advancing conversion count.
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

/** @brief Restrict this initial control surface to a useful subset of ADS1256 rates. */
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
 * @brief Queue one complete UART command and wait asynchronously for its matching reply.
 * @return False if disconnected, busy, invalid, or UART has insufficient buffer space.
 *
 * HTTP 202 only acknowledges transport acceptance. This function never sets running
 * state; only a validated S3 report can do that. USB/web polling cannot block waiting
 * for S3. The short command is buffered in full, and timeout handling runs in update().
 */
bool s3Monitor::sendCommand(const char* action, uint32_t rate)
{
    const uint32_t nowMs = millis();
    const bool isStart = strcmp(action, "start") == 0;
    const bool knownAction = isStart || strcmp(action, "stop") == 0 || strcmp(action, "reboot") == 0;
    bool retVal = false;
    if (knownAction && receivedFrame && nowMs - lastFrameMs < connectionTimeoutMs &&
        commandStatus != "pending" && (!isStart || (adcReady && validRate(rate) &&
        (!acquisitionRunning || rate == appliedRate))))
    {
        uint32_t nextId = commandId + 1;
        if (nextId == 0)
        {
            nextId = 1;
        }
        const String command = "CMD,2," + String(nextId) + "," + action + "," + String(isStart ? rate : 0) + "\n";
        if (serialPort.availableForWrite() >= static_cast<int>(command.length()))
        {
            retVal = serialPort.print(command) == command.length();
            if (retVal)
            {
                commandId = nextId;
                commandAction = action;
                requestedRate = rate;
                commandSentMs = nowMs;
                commandStatus = "pending";
            }
        }
    }
    return retVal;
}
