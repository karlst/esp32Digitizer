/**
 * @file recordingMonitor.cpp
 * @brief Convert validated recording numbers to exact decimal JSON strings.
 */
#include "recordingMonitor.h"
#include <cstdio>

/**
 * @brief Supply P3 with a recording object, or null if unsupported/disconnected.
 * All values are strings, including byte totals larger than JavaScript's exact
 * integer range. The browser can use BigInt for totals and bounded Number values
 * for percentages. File paths are generated from numeric IDs, not UART text.
 *
 * Called when Feather builds the HTTP status response for the browser.
 * connected says whether recent valid S3 reports are arriving; available says
 * whether those reports use a protocol version that includes recording fields.
 * Both must be true. Returning JSON null otherwise prevents old stored numbers
 * from being displayed as live recording state. This function never asks S3 to
 * start/stop recording and never reads the card.
 * JSON is the named-field text format sent to JavaScript. Even small fields are
 * quoted strings for a consistent schema. JavaScript Number cannot represent all
 * 64-bit integer totals exactly, so totals must not be sent as unquoted numbers.
 */
String recordingMonitor::stateJson(bool connected) const
{
    String retVal = "null";
    if (connected && available)
    {
        retVal = "{";
        // Translate the shared numeric field order into named JSON properties.
        // The fixed field names and decimal-only values need no arbitrary text escaping.
        for (size_t index = 0; index < recordingStatus::fieldCount; ++index)
        {
            // An unsigned 64-bit number needs at most 20 digits plus a terminator.
            // Format through an unsigned long long to match the %llu conversion.
            char value[24];
            snprintf(value, sizeof(value), "%llu", static_cast<unsigned long long>(status.values[index]));
            if (index) { retVal += ','; }
            retVal += "\"" + String(recordingStatus::fieldName(index)) + "\":\"" + String(value) + "\"";
        }
        retVal += '}';
    }
    return retVal;
}
