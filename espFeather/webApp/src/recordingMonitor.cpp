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
 * @file RecordingMonitor.cpp
 * @brief Convert validated recording numbers to exact decimal JSON strings.
 */
#include "RecordingMonitor.h"
#include <cstdio>

/**
 * @brief Supply the browser's recording panel with status, or null if unavailable.
 * All values are strings, including byte totals larger than JavaScript's exact
 * integer range. The browser can use BigInt for totals and bounded Number values
 * for percentages. File paths are generated from numeric IDs, not UART text.
 *
 * Called when Feather builds the HTTP status response for the browser.
 * connected says whether recent valid receiver reports are arriving; available says
 * whether those reports use a protocol version that includes recording fields.
 * Both must be true. Returning JSON null otherwise prevents old stored numbers
 * from being displayed as live recording state. This function never asks receiver to
 * start/stop recording and never reads the card.
 * JSON is the named-field text format sent to JavaScript. Even small fields are
 * quoted strings for a consistent schema. JavaScript Number cannot represent all
 * 64-bit integer totals exactly, so totals must not be sent as unquoted numbers.
 */
String RecordingMonitor::getStateJson(bool connected) const
{
    String retVal = "null";
    if (connected && _available)
    {
        retVal = "{";
        // Translate the shared numeric field order into named JSON properties.
        // The fixed field names and decimal-only values need no arbitrary text escaping.
        for (size_t index = 0; index < RecordingStatus::_fieldCount; ++index)
        {
            // An unsigned 64-bit number needs at most 20 digits plus a terminator.
            // Format through an unsigned long long to match the %llu conversion.
            char value[24];
            snprintf(value, sizeof(value), "%llu", static_cast<unsigned long long>(_recordingStatus._values[index]));
            if (index) { retVal += ','; }
            retVal += "\"" + String(RecordingStatus::getFieldName(index)) + "\":\"" + String(value) + "\"";
        }
        retVal += '}';
    }
    return retVal;
}
