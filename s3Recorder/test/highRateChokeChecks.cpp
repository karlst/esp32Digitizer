/**
 * @file highRateChokeChecks.cpp
 * @brief Verify 24-Mbps initial pacing, boundaries and sample contents on a PC.
 */
#include "chokeTest.h"
#include "chokeSink.h"
#include <cstdio>
static uint64_t nowUs = 0;

/** @brief Return the simulated clock; this test never waits for real sample time. */
static uint64_t clockUs() { return nowUs; }

/**
 * @brief Check cumulative counts across two rate changes, then restart and lag.
 * The real source and writer feed a sink that checks every signed sample word.
 * One-millisecond steps keep the producer within its lateness allowance. Jumping
 * to 160 seconds separately verifies the 28-Mbps target without claiming the
 * generator actually produced the skipped samples. No board or card is used.
 */
int main()
{
    bufferedWriter writer;
    chokeSink sink;
    chokeTest source;
    writer.start(sink, clockUs);
    source.start(0);
    assert(source.targetBps == 24000000);
    for (uint64_t tick = 1; tick <= 20000; ++tick)
    {
        nowUs = tick * 1000;
        assert(source.feed(nowUs, writer));
        while (writer.queuedBytes() >= bufferedWriter::blockBytes)
        {
            assert(writer.pump());
        }
        if (tick == 10000)
        {
            assert(source.samples == 7500000 && source.targetBps == 24250000);
            assert(source.completedBps == 24000000);
        }
    }
    assert(source.samples == 15078125 && source.targetBps == 24500000);
    assert(source.completedBps == 24250000);
    assert(writer.finish() && sink.words == source.samples);
    source.start(0);
    assert(source.samples == 0 && source.targetBps == 24000000);
    assert(!source.feed(160000000, writer));
    assert(source.targetBps == 28000000 && source.result == chokeTest::producerBehind);
    std::puts("PASS: 24-Mbps start, exact counts and sample contents, rate boundaries, restart and lag.");
    return 0;
}
