/**
 * @file recordingConfig.h
 * @brief Recording policy and board configuration, separate from byte buffering.
 * Read this first when connecting a new acquisition component. Build profiles
 * select RAM/block sizes in platformio.ini; the reusable writer has no pin,
 * filesystem, sample-format or reservation knowledge.
 */
#pragma once
#include <cstdint>

#ifndef S3_PREALLOCATED_PAYLOAD_BYTES
#define S3_PREALLOCATED_PAYLOAD_BYTES 0ULL
#endif
#ifndef S3_SD_CLOCK_KHZ
#define S3_SD_CLOCK_KHZ 20000
#endif
#ifndef S3_SD_WIDTH
#define S3_SD_WIDTH 4
#endif

/**
 * @brief Immutable project settings read before any acquisition starts.
 * reservePayloadBytes=0 preserves legacy growing, split files. A nonzero value
 * reserves ONE contiguous exFAT file before Start succeeds, limits accepted
 * payload to that size, and releases unused space on Stop. It is a byte budget,
 * not a duration; acquisition decides when to stop. No reservation fallback is
 * allowed. The deployed long-duration policy remains a separate decision.
 *
 * Current file metadata describes signed four-byte words. The ring accepts any
 * bytes, but a future three-byte formatter must also update the file format and
 * sample-count reporting; changing just this constant would mislabel recordings.
 */
namespace recordingConfig
{
    constexpr uint64_t reservePayloadBytes = S3_PREALLOCATED_PAYLOAD_BYTES;
    constexpr uint32_t storedSampleBytes = 4;
    constexpr uint32_t diskTaskCore = 1;
    constexpr uint32_t diskTaskPriority = 1;
    constexpr uint32_t diskTaskStackBytes = 6144;
    constexpr uint32_t sdClockKhz = S3_SD_CLOCK_KHZ;
    constexpr uint32_t sdWidth = S3_SD_WIDTH;
    constexpr int clockPin = 5, commandPin = 7;
    constexpr int data0Pin = 6, data1Pin = 15, data2Pin = 16, data3Pin = 4;
}
