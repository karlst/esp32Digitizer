/**
 * @file writerChecks.cpp
 * @brief Exercise the real byte writer without Arduino, a card, or an ADC.
 * Check ordering through wraps, stalled storage, partial writes, flush failure,
 * format bytes, and concurrent producer/consumer publication on separate threads.
 */
#include <bufferedWriter.h>
#include "sampleFormatter.h"
#include "memorySink.h"
#include <cassert>
#include <vector>
#include <thread>
#include <iostream>

uint64_t writerTestUs = 0;
/** @brief Deterministic clock used only by the consumer. */
static uint64_t clockUs() { return writerTestUs; }

/** @brief Run deterministic failures followed by concurrent byte-order stress. */
int main()
{
    bufferedWriter writer;
    memorySink sink;
    writer.start(sink, clockUs);
    sampleFormatter::submit(writer, -1);
    sampleFormatter::submit(writer, -8388608);
    sampleFormatter::submit(writer, 8388607);
    assert(writer.finish());
    const std::vector<uint8_t> expected = {255,255,255,255, 0,0,128,255, 255,255,127,0};
    assert(sink.bytes == expected);
    assert(writer.snapshot().maxUs == 45000 && writer.snapshot().maxKind == 2);
    const auto stoppedTime = writer.snapshot().elapsedUs;
    writerTestUs += 100000;
    assert(writer.snapshot().elapsedUs == stoppedTime);

    // Force a full buffer while its first block is still in write(). Queued
    // bytes must remain stable; overflow is visible and finish reports failure.
    sink.bytes.clear(); writer.start(sink, clockUs);
    for (int index = 0; index < 1024; ++index) { sampleFormatter::submit(writer, index); }
    sink.duringWrite = &writer;
    writer.pump();
    assert(writer.failed() && writer.snapshot().overflows == 1);
    assert(!writer.finish() && writer.snapshot().used == 0);

    // Never retry a partially written block. Its accepted prefix is counted,
    // but the queue remains occupied so callers cannot assume everything saved.
    sink.bytes.clear(); sink.shortWrite = true;
    writer.start(sink, clockUs); sampleFormatter::submit(writer, 42);
    assert(!writer.finish());
    assert(sink.bytes.size() == 3 && writer.snapshot().bytes == 3 && writer.snapshot().used == 4);
    assert(writer.snapshot().errors == 1);
    sink.shortWrite = false; sink.flushOk = false;
    writer.start(sink, clockUs);
    assert(!writer.finish() && writer.snapshot().errors == 1);
    writer.start(sink, clockUs);
    writerTestUs += 123;
    writer.measure(writerTestUs - 123, 1, false);
    writer.cancel();
    writerTestUs += 100000;
    assert(writer.snapshot().elapsedUs == 123);

    // A producer pauses BEFORE full capacity (it does not retry rejected data).
    // Real recording stops on full instead; this test isolates publication and
    // ordering through many ring wraps with actual concurrent threads.
    sink.flushOk = true; sink.bytes.clear(); writer.start(sink, clockUs);
    std::atomic<uint32_t> consumed{0};
    constexpr uint32_t count = 250000;
    std::thread producer([&]() {
        for (uint32_t index = 0; index < count; ++index)
        {
            while (index - consumed.load(std::memory_order_acquire) >= 8000) { std::this_thread::yield(); }
            assert(sampleFormatter::submit(writer, static_cast<int32_t>(index)));
        }
    });
    while (writer.snapshot().bytes < (count / 1024) * 4096)
    {
        assert(writer.pump());
        consumed.store(static_cast<uint32_t>(writer.snapshot().bytes / 4), std::memory_order_release);
        std::this_thread::yield();
    }
    producer.join();
    assert(writer.finish() && sink.bytes.size() == count * 4);
    for (uint32_t index = 0; index < count; ++index)
    {
        const uint8_t* bytes = sink.bytes.data() + index * 4;
        const uint32_t value = bytes[0] | (uint32_t(bytes[1]) << 8) |
            (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
        assert(value == index);
    }
    std::cout << "Writer checks passed: format, wraps, overflow, partial write, flush failure, concurrent order.\n";
    return 0;
}
