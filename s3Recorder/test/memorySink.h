/**
 * @file memorySink.h
 * @brief In-memory byte destination for the reusable writer's desktop tests.
 */
#pragma once
#include <bufferedWriter.h>
#include <vector>
#include <cassert>
extern uint64_t writerTestUs;

/**
 * @brief In-memory storage with controllable latency and failures.
 */
class memorySink : public byteSink
{
public:
    std::vector<uint8_t> bytes;
    bool shortWrite = false, flushOk = true;
    bufferedWriter* duringWrite = nullptr;

    /**
     * @brief Copy accepted bytes; optionally force producer activity while blocked.
     * This fake replaces an actual filesystem. bytes records what storage accepted;
     * shortWrite forces a one-byte-short result. duringWrite asks the producer to fill
     * the ring WHILE this write still owns its input memory. That deliberately stresses
     * the rule that tail must not advance before a write returns.
     */
    size_t write(const uint8_t* data, size_t length) override
    {
        // Retain a reference copy to detect whether concurrent submissions illegally
        // reused the block still being written.
        const std::vector<uint8_t> before(data, data + length);
        if (duringWrite)
        {
            const uint8_t sample[4] = {9, 8, 7, 6};
            while (duringWrite->submit(sample, 4)) {}

            // Buffer space must not be released until this call returns.
            assert(std::vector<uint8_t>(data, data + length) == before);
            duringWrite = nullptr;
        }

        // Pretend the write took 12 ms and accept either the full span or a short
        // prefix. The real writer should report exactly those simulated results.
        writerTestUs += 12000;
        const size_t retVal = shortWrite ? length - 1 : length;
        bytes.insert(bytes.end(), data, data + retVal);
        return retVal;
    }

    /**
     * @brief Make the final flush slower than writes to test retained maxima.
     * Advance simulated time, not real wall-clock time; no 45-ms sleep occurs.
     * Returning flushOk lets the test prove a failed synchronization is not hidden.
     */
    bool flush() override { writerTestUs += 45000; return flushOk; }
};

