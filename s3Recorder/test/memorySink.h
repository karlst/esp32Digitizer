/**
 * @file memorySink.h
 * @brief In-memory byte destination for the reusable writer's desktop tests.
 */
#pragma once
#include <bufferedWriter.h>
#include <vector>
#include <cassert>
extern uint64_t writerTestUs;

/** @brief In-memory storage with controllable latency and failures. */
class memorySink : public byteSink
{
public:
    std::vector<uint8_t> bytes;
    bool shortWrite = false, flushOk = true;
    bufferedWriter* duringWrite = nullptr;
    /** @brief Copy accepted bytes; optionally force producer activity while blocked. */
    size_t write(const uint8_t* data, size_t length) override
    {
        const std::vector<uint8_t> before(data, data + length);
        if (duringWrite)
        {
            const uint8_t sample[4] = {9, 8, 7, 6};
            while (duringWrite->submit(sample, 4)) {}
            // Buffer space must not be released until this call returns.
            assert(std::vector<uint8_t>(data, data + length) == before);
            duringWrite = nullptr;
        }
        writerTestUs += 12000;
        const size_t retVal = shortWrite ? length - 1 : length;
        bytes.insert(bytes.end(), data, data + retVal);
        return retVal;
    }
    /** @brief Make the final flush slower than writes to test retained maxima. */
    bool flush() override { writerTestUs += 45000; return flushOk; }
};

