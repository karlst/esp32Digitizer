/** @file writerRolloverChecks.cpp
 * @brief Exercise the actual ring past its 32-bit byte-position wrap, without disk I/O.
 * Memory use stays bounded: one ring and one block, not a multi-gigabyte file.
 */
#include "bufferedWriter.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>

/** @brief Mock destination verifies every block before the caller reuses its source. */
class rolloverSink : public byteSink
{
public:
    const uint8_t* expected = nullptr;
    size_t expectedBytes = 0;
    uint64_t total = 0;

    /** @brief Compare copied ring data with the requested block and count accepted bytes. */
    size_t write(const uint8_t* data, size_t length) override
    {
        assert(length == expectedBytes && std::memcmp(data, expected, length) == 0);
        total += length;
        return length;
    }

    /** @brief No hardware is present; synchronization succeeds in this boundary test. */
    bool flush() override { return true; }
};

/** @brief Provide deterministic timing; this test measures ordering, not throughput. */
static uint64_t clockUs() { return 1; }

/** @brief Use host heap for the external ring, matching the firmware's ownership model. */
static uint8_t* allocate(uint32_t bytes) { return static_cast<uint8_t*>(std::malloc(bytes)); }

/** @brief Release the host allocation when the writer goes out of scope. */
static void release(uint8_t* memory) { std::free(memory); }

/** @brief Transfer slightly over 4 GiB, checking actual wrap and 64-bit saved-byte totals. */
int main()
{
    bufferedWriter writer;
    assert(writer.initializeMemory(allocate, release));
    static uint8_t block[bufferedWriter::blockBytes];
    std::memset(block, 0x5a, sizeof(block));
    rolloverSink sink;
    sink.expected = block;
    sink.expectedBytes = sizeof(block);
    writer.start(sink, clockUs);
    const uint32_t blocks = static_cast<uint32_t>((uint64_t{1} << 32) / sizeof(block)) + 1;
    for (uint32_t index = 0; index < blocks; ++index)
    {
        // Distinguish successive blocks to expose a stale block at wraparound.
        std::memcpy(block, &index, sizeof(index));
        assert(writer.submit(block, sizeof(block)));
        assert(writer.pump());
    }
    sink.expectedBytes = 3;
    assert(writer.submit(block, 3));
    assert(writer.finish());
    const uint64_t expected = (uint64_t{1} << 32) + sizeof(block) + 3;
    const auto stats = writer.snapshot();
    assert(sink.total == expected && stats.bytes == expected);
    assert(stats.used == 0 && stats.unsavedBytes == 0 && stats.errors == 0 && stats.overflows == 0);
    puts("PASS: actual ring crossed 4 GiB position wrap; byte contents and 64-bit totals match.");
    return 0;
}
