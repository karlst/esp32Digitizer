/** @file sdWriteTest.cpp
 * @brief Paced 60-second disk test, with failure rather than silent throttling.
 * A task notification wakes the otherwise sleeping generator. The prepared
 * recorder is exclusively claimed before that notification; ADC ARM then fails.
 */
#include "sdWriteTest.h"
#include "sdTestData.h"
#include "esp_timer.h"
#include "esp_random.h"
#include <cstdio>

/** @brief Install the CPU1 generator once; creation does not start a test.
 * ESP-IDF specifies stack size in bytes. Reserve 8 KiB for nested library and
 * console calls; the separate member batch no longer consumes that stack.
 */
bool sdWriteTest::initialize(sdRecorder& recorder)
{
    destination = &recorder;
    return xTaskCreatePinnedToCore(taskEntry, "sdTest", 8192, this, 5, &task, 1) == pdPASS;
}

/** @brief CPU0 claims a prepared file and publishes the chosen word rate to CPU1. */
bool sdWriteTest::start(uint32_t wordsPerSecond)
{
    bool retVal = !busy.load() && (wordsPerSecond == 812500 || wordsPerSecond == 1024000);
    if (retVal)
    {
        retVal = destination->claim(esp_random() | 1U, sdTestData::stage, wordsPerSecond);
    }
    if (retVal)
    {
        rate = wordsPerSecond;
        stopRequested.store(false);
        busy.store(true);
        xTaskNotifyGive(task);
    }
    return retVal;
}

/** @brief CPU0 requests an early stop; partial tests cannot receive a pass verdict. */
void sdWriteTest::stop()
{
    if (busy.load()) { stopRequested.store(true); }
}

/** @brief FreeRTOS entry forwards to the generator's lifetime loop. */
void sdWriteTest::taskEntry(void* context)
{
    static_cast<sdWriteTest*>(context)->work();
}

/** @brief Submit exactly the timed number of words, or fail promptly.
 * A sample here is a generated four-byte test word, not an ADC conversion.
 * Every loop computes the cumulative deadline from the same starting timestamp.
 * The generator never waits for free ring space: submit rejection ends the test.
 * More than 100 ms of scheduling backlog also fails rather than pretending a
 * slower source sustained the requested rate. One-tick sleeps allow CPU1 idle.
 */
void sdWriteTest::work()
{
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // Integer formatting avoids the floating-point formatter's startup
        // allocation and deeper call stack. This is outside the timed interval.
        const uint32_t bitsPerSecond = rate * 32U;
        printf("SD TEST START %lu.%03lu Mbps for 60 seconds; generated data, ADC not used\n",
               static_cast<unsigned long>(bitsPerSecond / 1000000),
               static_cast<unsigned long>((bitsPerSecond % 1000000) / 1000));
        const int64_t began = esp_timer_get_time();
        const uint64_t expected = sdTestData::dueWords(sdTestData::durationUs, rate);
        uint64_t sent = 0;
        uint64_t maxLagUs = 0;
        bool ok = true;
        while (sent < expected && ok)
        {
            const uint64_t elapsed = esp_timer_get_time() - began;
            const uint64_t due = sdTestData::dueWords(elapsed, rate);
            const uint64_t lagUs = (due - sent) * 1000000 / rate;
            if (lagUs > maxLagUs) { maxLagUs = lagUs; }
            ok = !stopRequested.load() && !destination->failed() && lagUs <= 100000 &&
                 elapsed <= sdTestData::durationUs + 100000;
            while (sent < due && ok)
            {
                const size_t count = due - sent < 512 ? static_cast<size_t>(due - sent) : 512;
                for (size_t index = 0; index < count; ++index)
                {
                    batch[index] = sdTestData::word(static_cast<uint32_t>(sent + index));
                }
                ok = !stopRequested.load() && destination->submitWords(batch, count);
                if (ok) { sent += count; }
            }
            if (sent < expected && ok) { vTaskDelay(1); }
        }
        const uint64_t elapsed = esp_timer_get_time() - began;
        ok = ok && sent == expected && elapsed <= sdTestData::durationUs + 100000 &&
             !stopRequested.load() && !destination->failed();
        printf("SD TEST source %s words=%llu/%llu elapsedMs=%llu maxLagUs=%llu; "
               "wait for SD SAVED/FAILED\n", ok ? "COMPLETE" : "FAILED",
               static_cast<unsigned long long>(sent), static_cast<unsigned long long>(expected),
               static_cast<unsigned long long>(elapsed / 1000), static_cast<unsigned long long>(maxLagUs));
        // No further submissions follow publication of finishing. Readback and
        // file close belong to the disk task; source completion is not SD success.
        destination->finish(ok);
        busy.store(false);
    }
}
