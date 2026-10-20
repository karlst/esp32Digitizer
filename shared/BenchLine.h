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
 * @file BenchLine.h
 * @brief Assemble serial commands for Feather and P4, one character at a time.
 */
#pragma once
#include <cstddef>

/**
 * @brief Collect UART characters until newline; discard oversized lines entirely.
 * One control task/loop on each board owns its instance, so no lock is required. A true
 * feed() return exposes one complete null-terminated line until the next feed().
 */
class BenchLine
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
            _text[_length] = '\0';
            retVal = !_overflow && _length != 0;
            _length = 0;
            _overflow = false;
        }
        else if (value != '\r' && !_overflow)
        {
            if (_length + 1 < sizeof(_text))
            {
                _text[_length++] = value;
            }
            else
            {
                _overflow = true;
            }
        }
        return retVal;
    }

    /** @brief Return the last complete line; caller must use it before feeding more. */
    const char* value() const { return _text; }
private:
    char _text[384] = {};
    size_t _length = 0;
    bool _overflow = false;
};
