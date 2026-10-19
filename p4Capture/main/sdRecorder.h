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

/** @file sdRecorder.h
 * @brief P4 board adapter around the reusable buffered writer.
 * CPU1 packs ADC samples to three bytes; synthetic tests retain four-byte words.
 * A CPU0 disk task alone owns the file and card.
 * Read prepareFile(), work(), then finishFile() for the file lifecycle.
 */
#pragma once
#include <atomic>
#include "bufferedWriter.h"
#include "ff.h"
#include "sdmmc_cmd.h"
#include "sdkconfig.h"
#if CONFIG_P4_CAPTURE_PROFILE
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

/** @brief One prepared recording, with explicit asynchronous preparation and saving.
 * State publication transfers ownership: ready permits claim(), running permits
 * CPU1 submissions, finishing is published only after CPU1 has drained. Disk
 * calls never occur in either submission method. Files are created exclusively,
 * never replaced.
 * bufferedWriter owns the ring and protects queued bytes until disk writing
 * finishes. This adapter owns board/card configuration and file metadata.
 * prepare()/finish() request work by changing atomic state; their return does
 * not mean the asynchronous disk work is complete. Console READY/SAVED/FAILED
 * is printed by the disk task after the corresponding operation finishes.
 */
class sdRecorder : public byteSink
{
public:
    bool initialize();
    bool prepare();
    bool claim(uint32_t run, uint32_t stage, uint32_t rate);
    bool submitWords(const uint32_t* words, size_t count);
    bool submitSamples(const uint32_t* words, size_t count);
    bool failed() const;
    void finish(bool captureClean, uint32_t expectedSamples = 0);
#if CONFIG_P4_CAPTURE_PROFILE
    TaskHandle_t profileTask() const;
#endif
private:
#if CONFIG_P4_CAPTURE_PROFILE
    TaskHandle_t diskTask = nullptr;
#endif
    enum class phase { idle, preparing, ready, running, finishing, complete, error, blocked };
    static void taskEntry(void* context);
    void work();
    bool prepareFile();
    void finishFile();
    size_t write(const uint8_t* data, size_t length) override;
    bool flush() override;
    bool submitBytes(const uint8_t* data, uint32_t bytes);
    std::atomic<phase> state{phase::idle};
    std::atomic<bool> storageBroken{false};
    bufferedWriter writer;
    FIL file = {};
    sdmmc_card_t* card = nullptr;
    uint8_t* scratch = nullptr;
    bool opened = false;
    bool clean = false;
    uint32_t runId = 0, stageId = 0, sampleRate = 0;
    uint32_t submittedCrc = 0;
    uint64_t submittedBytes = 0, writtenBytes = 0;
    uint64_t writeUs = 0;
    char path[32] = {};

    // CPU1's decoder exclusively owns this batch while capturing. Keep it in
    // the global object's internal RAM, not on the decoder's small task stack.
    uint8_t packedSamples[512 * 3] = {};
};
