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
