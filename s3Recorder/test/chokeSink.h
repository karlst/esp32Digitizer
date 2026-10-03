/**
 * @file chokeSink.h
 * @brief Check every synthetic word without keeping a large test file in RAM.
 */
#pragma once
#include <bufferedWriter.h>
#include <cassert>

/**
 * @brief A simulated disk that verifies byte order and can fail a write or flush.
 * Desktop tests call it through the real bufferedWriter. There is no physical
 * disk or elapsed I/O time, so these tests prove logic, not SD speed.
 */
class chokeSink : public byteSink
{
public:
    uint64_t words = 0;
    bool failWrite = false, flushOk = true;
    /**
     * @brief Verify sequential signed 24-bit values, including sign/wrap boundaries.
     * A failed write returns zero and consumes nothing, exercising the real
     * writer's fault latch without inventing a successful card transfer.
     */
    size_t write(const uint8_t* data, size_t length) override
    {
        size_t retVal = 0;
        if (!failWrite)
        {
            assert(length % 4 == 0);
            for (size_t offset = 0; offset < length; offset += 4)
            {
                const uint32_t raw = static_cast<uint32_t>(words) & 0xffffff;
                const uint32_t expected = raw & 0x800000 ? raw | 0xff000000 : raw;
                const uint32_t actual = data[offset] | (static_cast<uint32_t>(data[offset + 1]) << 8) |
                    (static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
                assert(actual == expected);
                ++words;
            }
            retVal = length;
        }
        return retVal;
    }
    /**
     * @brief Return the configured synchronization result without touching hardware.
     */
    bool flush() override { return flushOk; }
};
