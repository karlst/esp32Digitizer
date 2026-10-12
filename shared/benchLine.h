/**
 * @file benchLine.h
 * @brief Bounded line assembly for the temporary Feather/S3 bench protocol.
 */
#pragma once
#include <cstddef>

/**
 * @brief Collect UART characters until newline; discard oversized lines entirely.
 * Each board's Arduino loop owns its instance, so no lock is required. A true
 * feed() return exposes one complete null-terminated line until the next feed().
 */
class benchLine
{
public:
    /** @brief Add one received character; return true only for a complete valid line. */
    bool feed(char value)
    {
        bool retVal = false;
        if (value == '\n')
        {
            text[length] = '\0';
            retVal = !overflow && length != 0;
            length = 0;
            overflow = false;
        }
        else if (value != '\r' && !overflow)
        {
            if (length + 1 < sizeof(text))
            {
                text[length++] = value;
            }
            else
            {
                overflow = true;
            }
        }
        return retVal;
    }

    /** @brief Return the last complete line; caller must use it before feeding more. */
    const char* value() const { return text; }
private:
    char text[384] = {};
    size_t length = 0;
    bool overflow = false;
};
