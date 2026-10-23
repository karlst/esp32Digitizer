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

/**
 * @file SampleDecoder.cpp
 * Take queued raw blocks, reconstruct samples and submit them without disk I/O.
 * Read initialize, configure, decode, then drain. FrameDecoder::feed reads each
 * raw block. Its output callback submits batches before returning. A sample is
 * one ADC measurement; each output word contains 8 status bits and 24 value bits.
 */
#include "SampleDecoder.h"
#include "esp_timer.h"

/* **
 * SampleDecoder - Save the receiver and optional timing observer references.
 * They must remain alive while this decoder runs. Construction does not start
 * the decoding task.
 */
SampleDecoder::SampleDecoder(AdcReceiver& adcReceiver, RecordingObserver* recordingObserver)
    : _adcReceiver(adcReceiver), _recordingObserver(recordingObserver)
{
}

/* **
 * initialize - Core 0 creates the completion signal and Core 1 task once;
 * failures abort startup.
 */
void SampleDecoder::initialize()
{
    _frameDecoder.setOutput(recordWords, this);
    _drained = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(_drained != nullptr ? ESP_OK : ESP_ERR_NO_MEM);

    // 4096 bytes is the task's call stack, separate from raw data blocks.
    // Priority 10 and Core 1 preserve the existing decoder scheduling.
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(decodeTask, "decode", 4096, this, 10,
                        &_decoderTaskHandle, 1) == pdPASS
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);
}

/* **
 * configure - Select the destination and reset decoding before starting the
 * receiver. clocksPerFrame is the expected number of ADC clock edges between
 * frame starts. Call only while decoding from the previous run has finished.
 */
void SampleDecoder::configure(CaptureDestination& captureDestination, uint32_t clocksPerFrame)
{
    _captureDestination = &captureDestination;
    _frameDecoder.reset(clocksPerFrame);
}

/* **
 * decodeTask - FreeRTOS starts here on Core 1; context identifies the decoder
 * whose loop should run.
 */
void SampleDecoder::decodeTask(void* context)
{
    static_cast<SampleDecoder*>(context)->decode();
}

/* **
 * decode - Core 1's permanent task waits for raw blocks and decodes them in
 * order. initialize() creates this task once; decodeTask forwards its context
 * here. It never returns. Only this task changes the decoder during capture.
 * Core 0 may inspect or reset it only after asking this task to finish its
 * queued work. A null pointer in readyBlocks means "everything before this must
 * be finished." After seeing it, this task gives the drained semaphore to signal
 * completion. Ordinary blocks return to freeBlocks only AFTER decoder.feed()
 * reads them and recordWords() copies or summarizes the decoded numbers. Even if
 * an SD submission fails, no later code retains the raw pointer, so it can be
 * reused. portMAX_DELAY sleeps this task until a pointer is available, rather
 * than burning CPU time checking repeatedly. It does not stop DMA or Core 0.
 * This task is woken through a queue; it is NOT an interrupt handler.
 */
void SampleDecoder::decode()
{
    int64_t lastPauseAt = esp_timer_get_time();
    bool firstBlock = true;
    for (;;)
    {
        CaptureBlock* captureBlock = _adcReceiver.take();
        if (captureBlock != nullptr)
        {
            // Timestamp before decoding so queue wait includes ISR copy and
            // scheduling delay. Timing reads add overhead only once per block.
            const int64_t startedAt = esp_timer_get_time();
            if (firstBlock)
            {
                lastPauseAt = startedAt;
                firstBlock = false;
            }

            // feed() reconstructs words and calls recordWords() synchronously.
            // That path copies samples into the SD ring or updates the RAM
            // summary. Neither waits for SD or retains this raw block.
            _frameDecoder.feed(captureBlock->_data, captureBlock->_length);
            const int64_t finishedAt = esp_timer_get_time();
            if (_recordingObserver != nullptr)
            {
                _recordingObserver->decoded(startedAt - captureBlock->_arrivedAt, finishedAt - startedAt,
                    static_cast<uint32_t>(captureBlock->_length));
            }

            // Return the ADDRESS, not the data: the memory stays in place and
            // Core 0 is now allowed to overwrite it with another DMA chunk.
            _adcReceiver.release(captureBlock);

            // Keep background/idle scheduling opportunities even under backlog,
            // but at most once per 100 ms rather than every sixteen blocks.
            // Reset the deadline on the first block of each run. Normal
            // empty-queue waits also let idle tasks run. vTaskDelay(1) waits
            // one operating-system tick, whose duration is a build setting;
            // the 100000 comparison is in microseconds, not ticks.
            if (finishedAt - lastPauseAt >= 100000)
            {
                const int64_t pauseStartedAt = esp_timer_get_time();
                vTaskDelay(1);
                lastPauseAt = esp_timer_get_time();
                if (_recordingObserver != nullptr)
                {
                    _recordingObserver->paused(lastPauseAt - pauseStartedAt);
                }
            }
        }
        else
        {
            // The null marker follows all real blocks for the stopped run.
            // Signal Core 0 that decoder state and summaries can now be read.
            lastPauseAt = esp_timer_get_time();
            firstBlock = true;
            xSemaphoreGive(_drained);
        }
    }
}

/* **
 * recordWords - Submit count decoded ADC words to the selected destination on
 * Core 1. Each 32-bit word contains eight status bits and a 24-bit sample;
 * FrameDecoder has already checked the status. context points to this
 * SampleDecoder.
 *
 * The SD destination strips the status byte and copies samples into its ring.
 * The RAM destination updates its test summary. Neither keeps the words pointer
 * after returning. True means the destination accepted the batch, not that data
 * was written to disk. False marks decoder output as failed.
 */
bool SampleDecoder::recordWords(const uint32_t* words, size_t count, void* context)
{
    return static_cast<SampleDecoder*>(context)->_captureDestination->submit(words, count);
}

/* **
 * drain - Wait up to one second for Core 1 to finish queued decoding. Core 0
 * calls this after stopping the receiver. A null block is queued after all real
 * blocks; Core 1 signals completion when it reaches that marker.
 *
 * Return false if the wait times out. The caller must then avoid reading or
 * resetting decoder state while Core 1 may still be changing it.
 */
bool SampleDecoder::drain()
{
    _adcReceiver.markDrain();
    return xSemaphoreTake(_drained, pdMS_TO_TICKS(1000)) == pdTRUE;
}
