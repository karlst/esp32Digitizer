/**
 * @file sampleFormatter.cpp
 * @brief Explicit byte order prevents a later computer from guessing the file format.
 */
#include "sampleFormatter.h"

/**
 * @brief Encode and enqueue one sample, with no allocation, disk I/O, or waiting.
 * False means the writer rejected this complete four-byte record. This function
 * and the called submit() live in instruction RAM for the 30k interrupt path.
 */
bool BUFFERED_IRAM sampleFormatter::submit(bufferedWriter& writer, int32_t sample)
{
    const uint32_t word = static_cast<uint32_t>(sample);
    const uint8_t bytes[4] = {static_cast<uint8_t>(word), static_cast<uint8_t>(word >> 8),
        static_cast<uint8_t>(word >> 16), static_cast<uint8_t>(word >> 24)};
    const bool retVal = writer.submit(bytes, sizeof(bytes));
    return retVal;
}
