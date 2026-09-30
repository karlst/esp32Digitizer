/**
 * @file commandProtocol.cpp
 * @brief Strict version-2 command decoding and ADS1256 data representation.
 */
#include "commandProtocol.h"
#include <cstring>

/**
 * @brief Parse a complete unsigned decimal without signs, whitespace, or overflow.
 */
bool commandProtocol::unsignedNumber(const char* text, uint32_t& value)
{
    bool retVal = *text != '\0';
    uint32_t parsed = 0;
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
 * @brief Map the UI choices to DRATE bytes for a 7.68 MHz ADS1256 clock.
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
 * @brief Split exactly five fields and commit only a wholly valid command.
 */
bool commandProtocol::parse(char* line, acquisitionCommand& command)
{
    char* fields[5] = {line};
    size_t count = 1;
    bool retVal = true;
    for (char* cursor = line; *cursor; ++cursor)
    {
        if (*cursor == ',')
        {
            *cursor = '\0';
            if (count < 5)
            {
                fields[count++] = cursor + 1;
            }
            else
            {
                retVal = false;
            }
        }
    }
    acquisitionCommand parsed;
    retVal = retVal && count == 5 && std::strcmp(fields[0], "CMD") == 0 &&
        std::strcmp(fields[1], "2") == 0 && unsignedNumber(fields[2], parsed.id) &&
        parsed.id != 0 && unsignedNumber(fields[4], parsed.rate);
    if (retVal)
    {
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
            retVal = parsed.rate == 0;
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
 * @brief Reject oversized, expired, or corrupted frames through the next newline.
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
 * @brief Sign-extend the big-endian 24-bit two's-complement ADC result safely.
 */
int32_t commandProtocol::signedSample(const uint8_t* bytes)
{
    const uint32_t raw = (uint32_t(bytes[0]) << 16) | (uint32_t(bytes[1]) << 8) | bytes[2];
    const int32_t retVal = (raw & 0x800000) ? static_cast<int32_t>(raw) - 0x1000000 :
        static_cast<int32_t>(raw);
    return retVal;
}
