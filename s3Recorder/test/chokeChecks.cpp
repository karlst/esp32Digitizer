/**
 * @file chokeChecks.cpp
 * @brief Test exact ramp timing, stored sample bytes, overflow and source lateness.
 */
#include "chokeTest.h"
#include "chokeSink.h"
#include <cstdio>

static uint64_t nowUs = 0;
/**
 * @brief Let the actual byte writer measure the same simulated time as the source.
 */
static uint64_t clockUs() { return nowUs; }

/**
 * @brief Simulate fast storage, checking rate boundaries and over 8 million words.
 * One-millisecond calls model the worker's normal scheduling. The cumulative rate
 * calculation must remain exact; it must not lose half-words by rounding each tick.
 */
static void checkRamp()
{
    bufferedWriter writer;
    chokeSink sink;
    chokeTest source;
    nowUs = 0;
    writer.start(sink, clockUs);
    source.start(nowUs);
    for (uint64_t tick = 1; tick <= 130000; ++tick)
    {
        nowUs = tick * 1000;
        assert(source.feed(nowUs, writer));
        while (writer.snapshot().used >= 4096)
        {
            assert(writer.pump());
        }
        if (tick == 10000)
        {
            assert(source.samples == 234375 && source.targetBps == 1000000 && source.completedBps == 750000);
        }
        if (tick == 20000)
        {
            assert(source.samples == 546875 && source.targetBps == 1250000 && source.completedBps == 1000000);
        }
    }
    assert(source.samples == 9140625 && source.latestSample < 0);
    source.stop(chokeTest::stopped);
    assert(writer.finish() && sink.words == source.samples);
    assert(!source.feed(nowUs + 1000, writer));
    assert(writer.snapshot().used == 0);
    // Check the target reaches 24 Mbps at 930 seconds, without pretending a
    // suddenly advanced clock means the generator actually sustained that rate.
    source.start(0);
    assert(!source.feed(930000000, writer));
    assert(source.targetBps == 24000000 && source.result == chokeTest::producerBehind);
}

/**
 * @brief A stopped consumer must overflow; the source must not throttle to hide it.
 * Once stopped, drain accepted data and verify the original failure remains set.
 */
static void checkOverflow()
{
    bufferedWriter writer;
    chokeSink sink;
    chokeTest source;
    nowUs = 0;
    writer.start(sink, clockUs);
    source.start(nowUs);
    bool accepted = true;
    for (uint32_t tick = 1; accepted && tick < 1000; ++tick)
    {
        nowUs = tick * 1000;
        accepted = source.feed(nowUs, writer);
    }
    assert(!accepted && source.result == chokeTest::storageLimit);
    assert(writer.snapshot().overflows == 1 && writer.snapshot().rejectedBytes != 0);
    assert(!writer.finish() && sink.words == source.samples);
    assert(writer.snapshot().used == 0);
}

/**
 * @brief Separate source lateness, failed writes, and failed final synchronization.
 * None of these may turn into a silently successful run or corrupt the ring order.
 */
static void checkFailures()
{
    bufferedWriter writer;
    chokeSink sink;
    chokeTest source;
    nowUs = 0;
    writer.start(sink, clockUs);
    source.start(0);
    assert(!source.feed(250000, writer));
    assert(source.result == chokeTest::producerBehind && !writer.failed() && source.samples == 0);
    source.start(0);
    nowUs = 50000;
    assert(source.feed(nowUs, writer));
    sink.failWrite = true;
    assert(!writer.pump() && writer.failed());
    assert(!source.feed(nowUs + 1000, writer) && source.result == chokeTest::storageLimit);
    assert(!writer.finish());
    // A new Start must clear the prior failure and begin at the initial target.
    sink.failWrite = false;
    writer.start(sink, clockUs);
    source.start(nowUs);
    assert(source.targetBps == 750000 && source.completedBps == 0 && source.samples == 0);
    assert(source.feed(nowUs + 1000, writer));
    sink.flushOk = false;
    assert(!writer.finish());
}

/**
 * @brief Run bounded desktop checks without serial access or physical card writes.
 */
int main()
{
    checkRamp();
    checkOverflow();
    checkFailures();
    std::puts("PASS: Choke ramp timing, sample encoding/sign, no throttling, overflow, write/flush failure, lag and restart.");
    return 0;
}
