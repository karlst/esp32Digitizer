/**
 * @file timedChokeChecks.cpp
 * @brief Simulate a timed fixed-rate run and verify every generated word.
 * Two simulated seconds cover rate pacing and the deadline without generating
 * a full card's worth of data. The final tick deliberately arrives late.
 */
#include "chokeTest.h"
#include "chokeSink.h"
#include <cstdio>
static uint64_t nowUs = 0;

/** @brief Supply the writer's simulated microsecond clock; no hardware is used. */
static uint64_t clockUs() { return nowUs; }

/** @brief Check exact deadline count, constant rate, clean drain and restart. */
int main()
{
    bufferedWriter writer;
    chokeSink sink;
    chokeTest source;
    writer.start(sink, clockUs);
    source.start(0);
    for (uint64_t tick = 1; tick < 2000; ++tick)
    {
        nowUs = tick * 1000;
        assert(source.feed(nowUs, writer));
        assert(source.targetBps == 26000000);
        while (writer.snapshot().used >= 4096)
        {
            assert(writer.pump());
        }
    }
    nowUs = 2000300;
    assert(!source.feed(nowUs, writer));
    assert(source.result == chokeTest::stopped);
    assert(source.elapsedMs == 2000 && source.samples == 1625000);
    assert(source.completedBps == 26000000);
    assert(writer.finish() && sink.words == 1625000);
    assert(writer.snapshot().used == 0);
    source.start(nowUs);
    assert(source.result == chokeTest::running && source.samples == 0);
    std::puts("PASS: fixed 26 Mbps, exact timed stop despite late tick, every word checked, clean drain/restart.");
    return 0;
}
