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
 */
String recordingMonitor::stateJson(bool connected) const
{
    String retVal = "null";
    if (connected && available)
    {
        retVal = "{";
        for (size_t index = 0; index < recordingStatus::fieldCount; ++index)
        {
            char value[24];
            snprintf(value, sizeof(value), "%llu", static_cast<unsigned long long>(status.values[index]));
            if (index) { retVal += ','; }
            retVal += "\"" + String(recordingStatus::fieldName(index)) + "\":\"" + String(value) + "\"";
        }
        retVal += '}';
    }
    return retVal;
}
