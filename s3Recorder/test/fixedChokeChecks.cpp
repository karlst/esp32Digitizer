/**
 * @file fixedChokeChecks.cpp
 * @brief Check the fixed-rate source over the entire 150-second width test.
 * Uses simulated time and a sink that checks every sample word; no hardware.
 */
#include "chokeTest.h"
#include "chokeSink.h"
#include <cstdio>

static uint64_t nowUs = 0;

/** @brief Give the writer the same simulated microsecond clock as the source. */
static uint64_t clockUs() { return nowUs; }

/** @brief Verify exact byte count, constant rate, sample contents and clean drain. */
int main()
{
    bufferedWriter writer;
    chokeSink sink;
    chokeTest source;
    writer.start(sink, clockUs);
    source.start(0);

    // Cross all ten-second boundaries so a mistaken ramp or count reset fails.
    for (uint64_t tick = 1; tick <= 150000; ++tick)
    {
        nowUs = tick * 1000;
        assert(source.feed(nowUs, writer));
        assert(source.targetBps == 250000 && source.completedBps == 0);
        while (writer.snapshot().used >= 4096)
        {
            assert(writer.pump());
        }
    }
    assert(source.samples == 1171875);
    source.stop(chokeTest::stopped);
    assert(writer.finish() && sink.words == 1171875);
    assert(writer.snapshot().used == 0);
    std::puts("PASS: fixed 250 kbps for 150 seconds, exact sample contents/count, clean drain.");
    return 0;
}
