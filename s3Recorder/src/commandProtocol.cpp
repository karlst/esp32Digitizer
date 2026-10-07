/**
 * @file commandProtocol.cpp
 * @brief Turn incoming serial text into validated commands for the acquisition task.
 *
 * featherLink.cpp calls feed() for each received character. feed() waits for a
 * complete newline-terminated message, then parse() checks every field. Neither
 * function touches hardware or executes the request; the caller queues accepted
 * commands for acquisition.cpp. A separate parser is used for each serial port.
 *
 * Command example: CMD,2,42,start,1000 means version 2, request ID 42, Start at
 * 1000 samples/second. Version 3 adds a sixth field: 1 records, 0 monitors only.
 * This file also maps rates to chip register codes and joins three sample bytes
 * into a signed number. Those helpers have no dependency on an attached board.
 */
#include "commandProtocol.h"
#include <cstring>

/**
 * @brief Read a nonempty string of decimal digits into a 32-bit unsigned integer.
 * @param text Null-terminated text; signs, spaces, fractions and exponents fail.
 * @param value Receives the parsed number only on success; unchanged on failure.
 * @return True only if every character is valid and the number fits in 32 bits.
 */
bool commandProtocol::unsignedNumber(const char* text, uint32_t& value)
{
    bool retVal = *text != '\0';
    uint32_t parsed = 0;

    // Check the limit BEFORE multiplying by ten; otherwise overflow could wrap
    // a huge input into an apparently valid small command ID or rate.
    for (size_t index = 0; text[index] && retVal; ++index)
    {
        const char digit = text[index];
        if (digit < '0' || digit > '9' || parsed > (UINT32_MAX - (digit - '0')) / 10)
        {
            retVal = false;
        }
        else
        {
            parsed = parsed * 10 + (digit - '0');
        }
    }
    if (retVal)
    {
        value = parsed;
    }
    return retVal;
}

/**
 * @brief Translate a supported samples-per-second choice to the ADS1256 DRATE byte.
 * @param rate Sample production rate, not the SPI bit-transfer clock speed.
 * @param value Receives the chip-specific code on success; unchanged on failure.
 * @return False for any rate outside our seven supported choices.
 *
 * The mapping assumes the digitizer has its standard 7.68 MHz clock. The register
 * byte is an encoded setting, not the decimal rate truncated to eight bits.
 */
bool commandProtocol::rateRegister(uint32_t rate, uint8_t& value)
{
    bool retVal = true;
    switch (rate)
    {
        case 100: value = 0x82; break;
        case 500: value = 0x92; break;
        case 1000: value = 0xa1; break;
        case 2000: value = 0xb0; break;
        case 7500: value = 0xd0; break;
        case 15000: value = 0xe0; break;
        case 30000: value = 0xf0; break;
        default: retVal = false; break;
    }
    return retVal;
}

/**
 * @brief Validate CMD,2,id,action,rate or CMD,3,id,action,rate,record.
 * @param line Writable, null-terminated text without its newline. Commas are
 * replaced with string terminators, so the original text is modified.
 * @param command Receives the complete request only if every check succeeds.
 * @return True for an accepted format; false leaves command unchanged.
 *
 * IDs must be nonzero. Start requires a supported rate; Stop/Reboot require zero
 * in that field. Delete requires version 3 and zero rate/record fields.
 * A syntactically valid command can still be rejected later by
 * acquisition.cpp because the digitizer is unavailable or already running.
 */
bool commandProtocol::parse(char* line, acquisitionCommand& command)
{
    // Split without allocating strings. Empty fields remain visible to validation;
    // extra commas are an error rather than ignored trailing data.
    char* fields[6] = {line};
    size_t count = 1;
    bool retVal = true;
    for (char* cursor = line; *cursor; ++cursor)
    {
        if (*cursor == ',')
        {
            *cursor = '\0';
            if (count < 6)
            {
                fields[count++] = cursor + 1;
            }
            else
            {
                retVal = false;
            }
        }
    }

    // Work on a temporary request so a valid prefix cannot partially update the
    // caller's command when a later field is invalid.
    acquisitionCommand parsed;
    const bool version3 = count == 6 && std::strcmp(fields[1], "3") == 0;
    const bool version2 = count == 5 && std::strcmp(fields[1], "2") == 0;
    uint32_t recordFlag = 0;
    retVal = retVal && (version2 || version3) && std::strcmp(fields[0], "CMD") == 0 &&
        (!version3 || (unsignedNumber(fields[5], recordFlag) && recordFlag <= 1)) &&
        unsignedNumber(fields[2], parsed.id) &&
        parsed.id != 0 && unsignedNumber(fields[4], parsed.rate);
    if (retVal)
    {
        // Rate lookup validates Start even though this parser does not write the
        // register. Stop/Reboot ignore settings operationally but require wire zero.
        parsed.record = recordFlag != 0;
        uint8_t ignored = 0;
        if (std::strcmp(fields[3], "start") == 0)
        {
            parsed.action = acquisitionCommand::Action::start;
            retVal = rateRegister(parsed.rate, ignored);
        }
        else if (std::strcmp(fields[3], "stop") == 0 || std::strcmp(fields[3], "reboot") == 0)
        {
            parsed.action = std::strcmp(fields[3], "stop") == 0 ?
                acquisitionCommand::Action::stop : acquisitionCommand::Action::reboot;
            retVal = parsed.rate == 0 && !parsed.record;
        }
        else if (version3 && std::strcmp(fields[3], "delete") == 0)
        {
            parsed.action = acquisitionCommand::Action::erase;
            retVal = parsed.rate == 0 && !parsed.record;
        }
        else
        {
            retVal = false;
        }
    }
    if (retVal)
    {
        command = parsed;
    }
    return retVal;
}

/**
 * @brief Add one serial character; return a command only at a valid line ending.
 * @param byte Next character read by featherLink from one serial port.
 * @param nowMs Current S3 time in milliseconds, for incomplete-message expiry.
 * @param command Written only when a full valid command has been assembled.
 * @return True for that completed command; false for partial or rejected input.
 *
 * LF is a newline; CRLF is also accepted, but CR anywhere else is invalid. A
 * partial message with more than 500 ms between characters is discarded. After
 * noise, overflow or timeout, discard through the next newline before starting
 * a new message. This prevents a broken message's tail becoming a new command.
 */
bool commandProtocol::feed(char byte, uint32_t nowMs, acquisitionCommand& command)
{
    bool retVal = false;

    // Unsigned subtraction also works when millis() rolls over after about 49 days.
    if ((length || carriageReturn) && nowMs - lastByteMs > 500)
    {
        discard = true;
    }
    lastByteMs = nowMs;

    // A newline finishes either the message or the discard period. In both
    // cases reset the parser so the next line starts independently.
    if (byte == '\n')
    {
        line[length] = '\0';
        if (!discard)
        {
            retVal = parse(line, command);
        }
        length = 0;
        discard = false;
        carriageReturn = false;
    }
    else if (!discard)
    {
        // Keep one slot for the final null terminator. Once CR is seen, only
        // LF may follow; accepting other characters would silently splice text.
        if (byte == '\r' && !carriageReturn)
        {
            carriageReturn = true;
        }
        else if (carriageReturn || byte < ' ' || byte > '~' || length >= sizeof(line) - 1)
        {
            discard = true;
        }
        else
        {
            line[length++] = byte;
        }
    }
    return retVal;
}

/**
 * @brief Assemble the digitizer's three bytes into one signed sample, not volts.
 * @param bytes At least three bytes in received order: highest-value byte first.
 * @return A signed value from -8388608 to 8388607, stored in a 32-bit integer.
 *
 * The top bit of the 24-bit result is its sign. If set, subtract 2^24 from the
 * unsigned value to recover the negative number (for example, 0xffffff is -1).
 * This avoids treating every negative input as a large positive sample.
 */
int32_t commandProtocol::signedSample(const uint8_t* bytes)
{
    const uint32_t raw = (uint32_t(bytes[0]) << 16) | (uint32_t(bytes[1]) << 8) | bytes[2];
    const int32_t retVal = (raw & 0x800000) ? static_cast<int32_t>(raw) - 0x1000000 :
        static_cast<int32_t>(raw);
    return retVal;
}
