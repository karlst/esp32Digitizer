/**
 * @file sampleFormatter.cpp
 * @brief Explicit byte order prevents a later computer from guessing the file format.
 */
#include "sampleFormatter.h"

/**
 * @brief Encode and enqueue one sample, with no allocation, disk I/O, or waiting.
 * False means the writer rejected this complete four-byte record. This function
 * and the called submit() live in instruction RAM for the 30k interrupt path.
 *
 * Called by fastCapture's interrupt at 30k, or acquisition::collect() at lower
 * rates, immediately after accepting ONE ADC measurement. sample is already a
 * signed 32-bit integer; the driver extended the original 24-bit sign into it.
 * This class defines how that number becomes file bytes. The disk writer knows
 * nothing about voltages, ADC bit counts, or the future meaning of GPS bits.
 * @param writer Already prepared byte writer belonging to the current recording.
 * @param sample Raw ADC count, not a voltage and not a pointer to an array.
 * @return True means all four bytes entered RAM. Disk completion is reported later.
 * For example -1 becomes FF FF FF FF; 0x123456 becomes 56 34 12 00.
 * BUFFERED_IRAM places executable instructions in internal RAM on ESP32, so the
 * interrupt does not depend on code being fetched from external flash.
 */
bool BUFFERED_IRAM sampleFormatter::submit(bufferedWriter& writer, int32_t sample)
{
    // Convert to unsigned before shifting, preserving the two's-complement bit
    // pattern of negative values. This avoids signed-right-shift ambiguity.
    const uint32_t word = static_cast<uint32_t>(sample);

    // Little endian means least-significant byte FIRST. Explicit shifts make
    // the file order clear rather than depending on the computer's memory layout.
    const uint8_t bytes[4] = {static_cast<uint8_t>(word), static_cast<uint8_t>(word >> 8),
        static_cast<uint8_t>(word >> 16), static_cast<uint8_t>(word >> 24)};

    // submit copies these local bytes before returning; the ring does not keep
    // a pointer to this temporary stack array. Failure rejects the entire record.
    const bool retVal = writer.submit(bytes, sizeof(bytes));
    return retVal;
}
