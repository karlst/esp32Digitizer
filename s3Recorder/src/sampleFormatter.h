/**
 * @file sampleFormatter.h
 * @brief Encode ADC values before they reach the storage-independent byte writer.
 */
#pragma once
#include <bufferedWriter.h>

/**
 * @brief Version 1 encoding: signed 32-bit little-endian words, one per sample.
 * The ADS1256 driver already sign-extends its 24-bit value. Future bit packing or
 * GPS insertion belongs here, with a new format ID in the recording header.
 */
class sampleFormatter
{
public:
    // One measurement in, four copied bytes out. Static means no formatter object
    // or private state is required. See the .cpp for byte order and negative examples.
    static bool BUFFERED_IRAM submit(bufferedWriter& writer, int32_t sample);
};
