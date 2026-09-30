/**
 * @file commandProtocol.h
 * @brief Hardware-independent UART framing, command validation, and ADC value helpers.
 */
#pragma once
#include <cstddef>
#include <cstdint>

/**
 * @brief A fully validated request; invalid input never reaches the acquisition task.
 */
struct acquisitionCommand
{
    enum class Action { start, stop, reboot };
    uint32_t id = 0;
    Action action = Action::stop;
    uint32_t rate = 0;
};

/**
 * @brief Bounded newline receiver shared by firmware and native regression tests.
 * No allocation, blocking reads, or prefix acceptance of malformed numbers is used.
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
    char line[80] = {};
    size_t length = 0;
    uint32_t lastByteMs = 0;
    bool discard = false;
    bool carriageReturn = false;
};
