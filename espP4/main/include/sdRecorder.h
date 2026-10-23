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
 * @file SdRecorder.h
 * Asynchronous ADC adapter for the reusable file-ring backend.
 * Core 1 packs samples and submits RAM copies; the Core 0 disk task alone
 * prepares, writes and reads files through FileRecordingBackend.
 */
#pragma once
#include <atomic>
#include "FileRecordingBackend.h"
#include "RetainedPayloadChecksums.h"
#include "P4RecordingDisk.h"
#include "RecordingLayout.h"
#include "RecordingReadback.h"
#include "RecordingReporter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * Bridge acquisition requests to one persistent disk task.
 * Startup supplies file duration/count/preparation policy. prepare() copies run
 * parameters before publishing a request; READY confirms actual disk preparation.
 * submitBytes() never performs disk I/O. finish() is requested only after Core 1
 * stops; SAVED requires retained-stream readback and matching per-generation CRC/counts.
 * Circular sessions verify only surviving generations; whole-run reception/write
 * counters are still checked. Startup or idle configuration chooses the pool policy.
 * Standard headers describe storage completion, not analog or ADC validity; the
 * final reporter result additionally checks acquisition validity.
 */
class SdRecorder
{
public:
    explicit SdRecorder(const RecordingSessionConfig& recordingSessionConfig,
        RecordingReadback* recordingReadback = nullptr, RecordingReporter* recordingReporter = nullptr);
    bool initTask();
    bool configure(const RecordingSessionConfig& config);
    bool prepare(uint32_t run, uint32_t rate, const RecordingLayout* recordingLayout = nullptr);
    bool getReady() const;
    void cancelPreparation();
    bool submitBytes(const uint8_t* data, uint32_t bytes);
    bool submitSamples(const uint32_t* words, size_t count);
    bool getFailed() const;
    bool getFull() const;
    void finish(bool captureClean, uint32_t expectedSamples = 0);
    TaskHandle_t getTaskHandle() const;

private:
    enum class Phase
    {
        idle,
        preparing,
        running,
        finishing,
        complete,
        error,
        blocked
    };
    static void taskEntry(void* context);
    static bool checkReadback(void* context, const uint8_t* data, uint32_t bytes, uint64_t offset);
    void work();
    bool prepareFile();
    static void reportProgress(void* context, const char* text);
    void closeCancelledPreparation();
    void finishFile();
    RecordingReporter* _recordingReporter;
    RecordingReadback* _recordingReadback;
    RecordingSessionConfig _recordingSessionConfig;
    RecordingLayout _recordingLayoutDefault;
    const RecordingLayout* _recordingLayout = &_recordingLayoutDefault;
    FileRecordingBackend _fileRecordingBackend;
    RetainedPayloadChecksums _retainedPayloadChecksums;
    P4RecordingDisk _p4RecordingDisk;
    TaskHandle_t _diskTask = nullptr;
    std::atomic<Phase> _state{Phase::idle};
    std::atomic<bool> _storageBroken{false}, _preparationCancelled{false};
    bool _clean = false;
    uint32_t _runId = 0, _sampleRate = 0;
    uint32_t _readbackCrc = 0;
    uint64_t _submittedBytes = 0, _readbackBytes = 0, _patternErrors = 0;
    char _path[64] = {};

    // Core 1 owns this batch until submit() copies it into the backend ring.
    // Keep it outside the decoder's stack; the global recorder owns its lifetime.
    uint8_t _packedSamples[512 * 3] = {};
};
