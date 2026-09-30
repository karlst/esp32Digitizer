/**
 * @file commandProtocol.h
 * @brief Hardware-independent UART framing, command validation, and ADC value helpers.
 */
#pragma once
#include <cstddef>
#include <cstdint>

/**
 * @brief Plain values passed from the serial parser to the acquisition task.
 * The parser validates incoming text before filling this record. id lets the
 * sender match a later reply; rate is samples/second for Start and zero otherwise.
 * The default ID zero is not a valid incoming command.
 */
struct acquisitionCommand
{
    enum class Action { start, stop, reboot, erase };
    uint32_t id = 0;
    Action action = Action::stop;
    uint32_t rate = 0;
    bool record = false; // Only Start may enable recording; legacy commands leave it off.
};

/**
 * @brief Accumulate one serial line and turn it into a request only when fully valid.
 * One instance per input port prevents mixing partial USB and Feather messages.
 * feed() is called once per character, never waits for another character, and
 * uses a fixed buffer. parse() can also validate a complete writable line directly.
 * Desktop tests use this same implementation, with no hardware attached.
 */
class commandProtocol
{
public:
    bool feed(char byte, uint32_t nowMs, acquisitionCommand& command);
    static bool parse(char* line, acquisitionCommand& command);
    static bool rateRegister(uint32_t rate, uint8_t& value);
    static int32_t signedSample(const uint8_t* bytes);
private:
    static bool unsignedNumber(const char* text, uint32_t& value);
    // At most 79 characters plus a terminating zero. discard remains set until
    // newline after a bad/oversized/expired line; carriageReturn permits CRLF only.
    char line[80] = {};
    size_t length = 0;
    uint32_t lastByteMs = 0;
    bool discard = false;
    bool carriageReturn = false;
};
