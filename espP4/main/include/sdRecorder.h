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

/** @file SdRecorder.h
 * @brief P4 board adapter around the reusable buffered writer.
 * CPU1 packs ADC samples to three bytes; synthetic tests retain four-byte words.
 * A CPU0 disk task alone owns the file and card.
 * Read prepareFile(), work(), then finishFile() for the file lifecycle.
 */
#pragma once
#include <atomic>
#include "BufferedWriter.h"
#include "ff.h"
#include "sdmmc_cmd.h"
#include "RecordingLayout.h"
#include "RecordingReadback.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/** @brief One recording whose card operations run in a separate background task.
 * Requests change a state variable shared with that task: ready permits claim(),
 * running permits Core 1 submissions, and finishing means Core 1 has stopped
 * submitting and finished its queued work. Disk
 * calls never occur in either submission method. Files are created exclusively,
 * never replaced.
 * bufferedWriter owns the ring and protects queued bytes until disk writing
 * finishes. This adapter owns board/card configuration and file metadata.
 * prepare()/finish() request work by changing atomic state; their return does
 * not mean the asynchronous disk work is complete. Console READY/SAVED/FAILED
 * is printed by the disk task after the corresponding operation finishes.
 */
class SdRecorder : public ByteSink
{
public:
    explicit SdRecorder(uint64_t reservedBytes = 10000000000ULL, RecordingReadback* recordingReadback = nullptr);
    bool initialize();
    bool prepare();
    bool claim(uint32_t run, uint32_t stage, uint32_t rate, const RecordingLayout* recordingLayout = nullptr);
    bool submitBytes(const uint8_t* data, uint32_t bytes);
    bool submitSamples(const uint32_t* words, size_t count);
    bool failed() const;
    void finish(bool captureClean, uint32_t expectedSamples = 0);
    TaskHandle_t taskHandle() const;
private:
    const uint64_t _reservedBytes;
    RecordingReadback* _recordingReadback;
    RecordingLayout _recordingLayoutDefault;
    const RecordingLayout* _recordingLayout = &_recordingLayoutDefault;
    TaskHandle_t _diskTask = nullptr;
    enum class phase { idle, preparing, ready, running, finishing, complete, error, blocked };
    static void taskEntry(void* context);
    void work();
    bool prepareFile();
    void finishFile();
    size_t write(const uint8_t* data, size_t length) override;
    bool flush() override;
    std::atomic<phase> _state{phase::idle};
    std::atomic<bool> _storageBroken{false};
    BufferedWriter _bufferedWriter;
    FIL _file = {};
    sdmmc_card_t* _card = nullptr;
    uint8_t* _scratch = nullptr;
    bool _opened = false;
    bool _clean = false;
    uint32_t _runId = 0, _stageId = 0, _sampleRate = 0;
    uint32_t _submittedCrc = 0;
    uint64_t _submittedBytes = 0, _writtenBytes = 0;
    uint64_t _writeUs = 0;
    char _path[32] = {};

    // CPU1's decoder exclusively owns this batch while capturing. Keep it in
    // the global object's internal RAM, not on the decoder's small task stack.
    uint8_t _packedSamples[512 * 3] = {};
};
