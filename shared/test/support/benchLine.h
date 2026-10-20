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
 * @brief Assemble serial test commands for Feather, S3, and P4, one character at a time.
 */
#pragma once
#include <cstddef>

/**
 * @brief Collect UART characters until newline; discard oversized lines entirely.
 * One control task/loop on each board owns its instance, so no lock is required. A true
 * feed() return exposes one complete null-terminated line until the next feed().
 */
class benchLine
{
public:
    /**
     * @brief Add a character; true means a nonempty line ended at newline.
     * Ignore carriage return. Keep at most 383 characters; a longer line is
     * discarded through its newline, then collection starts fresh. "Valid"
     * here means it fits, not that its command syntax or contents are correct.
     */
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
