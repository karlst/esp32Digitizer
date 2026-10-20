/*
 * Copyright (C) 2026 Karl Stock
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of ESP32 Digitizer.
 * You may redistribute and/or modify it under the GNU General Public License
 * as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 *
 * Distributed WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License in the repository LICENSE file or <https://www.gnu.org/licenses/>.
 */

/** @file SdWriteTest.cpp
 * @brief Generate known numbers for 60 seconds to test SD writing without an ADC.
 * start() chooses a rate; work() computes how many words should exist by each
 * elapsed time. Falling too far behind fails the test instead of quietly
 * reducing the rate. A task notification is an OS signal that wakes the
 * otherwise sleeping generator. The prepared
 * recorder is exclusively claimed before that notification; ADC ARM then fails.
 */
#include "SdWriteTest.h"
#include "sdTestData.h"
#include "esp_timer.h"
#include "esp_random.h"
#include <cstdio>

/** @brief Install the CPU1 generator once; creation does not start a test.
 * ESP-IDF specifies stack size in bytes. Reserve 8 KiB for nested library and
 * console calls; the separate member batch no longer consumes that stack.
 */
bool SdWriteTest::initialize(SdRecorder& recorder)
{
    _sdRecorder = &recorder;
    return xTaskCreatePinnedToCore(taskEntry, "sdTest", 8192, this, 5, &_task, 1) == pdPASS;
}

/**
 * @brief Core 0 starts a generated-data test at one of two supported word rates.
 * wordsPerSecond must be 812500 or 1024000; each word is four bytes. Return
 * true only when an unused prepared file was claimed and the task was signaled.
 * This does not mean the test or card writes have completed successfully.
 */
bool SdWriteTest::start(uint32_t wordsPerSecond)
{
    bool retVal = !_busy.load() && (wordsPerSecond == 812500 || wordsPerSecond == 1024000);
    if (retVal)
    {
        retVal = _sdRecorder->claim(esp_random() | 1U, sdTestData::stage, wordsPerSecond, &_syntheticRecordingLayout);
    }
    if (retVal)
    {
        _rate = wordsPerSecond;
        _stopRequested.store(false);
        _busy.store(true);
        xTaskNotifyGive(_task);
    }
    return retVal;
}

/** @brief CPU0 requests an early stop; partial tests cannot receive a pass verdict. */
void SdWriteTest::stop()
{
    if (_busy.load()) { _stopRequested.store(true); }
}

/**
 * @brief OS entry receives the sdWriteTest pointer supplied by initialize().
 * Its work() loop waits for requests and never returns during normal operation.
 */
void SdWriteTest::taskEntry(void* context)
{
    static_cast<SdWriteTest*>(context)->work();
}

/** @brief Submit exactly the timed number of words, or fail promptly.
 * A sample here is a generated four-byte test word, not an ADC conversion.
 * Every loop computes the cumulative deadline from the same starting timestamp.
 * The generator never waits for free ring space: submit rejection ends the test.
 * More than 100 ms of scheduling backlog also fails rather than pretending a
 * slower source sustained the requested rate. One-tick sleeps allow CPU1 idle.
 */
void SdWriteTest::work()
{
    for (;;)
    {
        // Sleep until start() signals us. pdTRUE clears accumulated wakeups;
        // portMAX_DELAY waits indefinitely without consuming CPU in a poll loop.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // Integer formatting avoids the floating-point formatter's startup
        // allocation and deeper call stack. This is outside the timed interval.
        const uint32_t bitsPerSecond = _rate * 32U;
        printf("SD TEST START %lu.%03lu Mbps for 60 seconds; generated data, ADC not used\n",
               static_cast<unsigned long>(bitsPerSecond / 1000000),
               static_cast<unsigned long>((bitsPerSecond % 1000000) / 1000));
        const int64_t began = esp_timer_get_time();
        const uint64_t expected = sdTestData::dueWords(sdTestData::durationUs, _rate);
        uint64_t sent = 0;
        uint64_t maxLagUs = 0;
        bool ok = true;
        while (sent < expected && ok)
        {
            // Compare submitted words with words due by wall-clock time. The
            // difference converted to microseconds is how far the source lags.
            const uint64_t elapsed = esp_timer_get_time() - began;
            const uint64_t due = sdTestData::dueWords(elapsed, _rate);
            const uint64_t lagUs = (due - sent) * 1000000 / _rate;
            if (lagUs > maxLagUs) { maxLagUs = lagUs; }
            ok = !_stopRequested.load() && !_sdRecorder->failed() && lagUs <= 100000 &&
                 elapsed <= sdTestData::durationUs + 100000;
            while (sent < due && ok)
            {
                // Fill at most 512 words, each determined by its position.
                // submitBytes copies them, so this batch can be reused as soon
                // as it returns. Refusal ends the test; do not retry lost data.
                const size_t count = due - sent < 512 ? static_cast<size_t>(due - sent) : 512;
                for (size_t index = 0; index < count; ++index)
                {
                    _batch[index] = sdTestData::word(static_cast<uint32_t>(sent + index));
                }
                ok = !_stopRequested.load() && _sdRecorder->submitBytes(reinterpret_cast<const uint8_t*>(_batch),
                    static_cast<uint32_t>(count * sizeof(uint32_t)));
                if (ok) { sent += count; }
            }
            if (sent < expected && ok) { vTaskDelay(1); }
        }
        const uint64_t elapsed = esp_timer_get_time() - began;
        ok = ok && sent == expected && elapsed <= sdTestData::durationUs + 100000 &&
             !_stopRequested.load() && !_sdRecorder->failed();
        printf("SD TEST source %s words=%llu/%llu elapsedMs=%llu maxLagUs=%llu; "
               "wait for SD SAVED/FAILED\n", ok ? "COMPLETE" : "FAILED",
               static_cast<unsigned long long>(sent), static_cast<unsigned long long>(expected),
               static_cast<unsigned long long>(elapsed / 1000), static_cast<unsigned long long>(maxLagUs));
        // No further submissions follow publication of finishing. Readback and
        // file close belong to the disk task; source completion is not SD success.
        _sdRecorder->finish(ok);
        _busy.store(false);
    }
}

