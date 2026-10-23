// Copyright (C) 2026 Karl Stock
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file SdChokeTest.h
 * P4-only fresh/add-files/use-existing storage tests; no ADC or Feather is involved.
 * Read start(), run(), generate(), then FileRecordingBackend::pump(). Core 1
 * offers timed bytes; Core 0 runs the reusable session/file backend.
 * The console task reads short, locked snapshots and never calls FatFs.
 */
#pragma once
#include "FileRecordingBackend.h"
#include "P4RecordingDisk.h"
#include "PreservedFiles.h"
#include "sdkconfig.h"
#include "ff.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>

/**
 * Generate, store and verify a timed stream using preallocated files.
 * One test per reset keeps failed card initialization from being reused. start()
 * queues work, not proof of a prepared card. Only the disk task accesses files.
 * The numbered stream counts accepted bytes; rejected input is counted separately.
 * This tests storage integrity and throughput, not ADC capture or power-loss recovery.
 */
class SdChokeTest
{
public:
    /* **
     * SdChokeTest - Construct idle counters; neither allocate memory nor touch the
     * card.
     */
    SdChokeTest() = default;
    enum class Mode
    {
        fresh,
        addFiles,
        existing
    };
    bool start(Mode mode);
    bool setUtc(uint64_t seconds);
    void identify() const;
    void stop();
    void report();

private:
    enum class State
    {
        idle,
        preparing,
        running,
        draining,
        verifying,
        preserving,
        done
    };
    static constexpr uint32_t _fileCount = CONFIG_P4_CHOKE_FILE_COUNT;
    static constexpr uint32_t _secondsPerFile = CONFIG_P4_CHOKE_FILE_SECONDS;
    static constexpr uint32_t _runSeconds = CONFIG_P4_CHOKE_RUN_SECONDS;
    static constexpr uint32_t _bytesPerSecond = 3072000;
    static constexpr uint32_t _batchBytes = _bytesPerSecond / 100;
    // ESP-IDF task stack sizes are bytes. This covers preparation, filesystem
    // calls and reporting; it is separate from the 16-MiB recording ring.
    static constexpr uint32_t _diskStackBytes = 16384;
    static constexpr uint64_t _fileBytes = uint64_t(_secondsPerFile) * _bytesPerSecond;

    /**
     * Coherent reporting copy; all access is under the short snapshot lock.
     */
    struct Snapshot
    {
        uint64_t _scheduled = 0, _accepted = 0, _lost = 0, _late = 0;
        uint64_t _written = 0, _rolloverUs = 0, _writeUs = 0, _bufferHoldUs = 0;
        uint32_t _seconds = 0, _used = 0, _peak = 0, _fileNumber = 1, _rollovers = 0;
    };

    static void diskEntry(void* context);
    static void producerEntry(void* context);
    static uint64_t clockUs();
    static uint64_t utcSeconds();
    static bool checkPattern(const uint8_t* data, uint32_t length, uint64_t offset);
    void updateStorageSnapshot();
    static uint8_t* allocateRing(uint32_t bytes);
    static void releaseRing(uint8_t* memory);
    static void fillPattern(uint8_t* data, uint32_t length, uint64_t offset);
    void run();
    void generate();
    bool prepareExistingTest(RecordingSessionConfig& config);
    bool expectUsedPoolRefusal(RecordingSessionConfig& config);

    FileRecordingBackend _fileRecordingBackend;
    P4RecordingDisk _p4RecordingDisk;
    PreservedFiles _preservedFiles;
    PreservedFiles _preservedLayout;
    // Used-pool inspection is disk-task-only. Its embedded sector cache must
    // not occupy the task stack while calling into the filesystem/backend.
    FIL _inspectionFile = {};
    Snapshot _snapshot;
    portMUX_TYPE _snapshotMux = portMUX_INITIALIZER_UNLOCKED;
    std::atomic<State> _state{State::idle};
    std::atomic<bool> _stopRequested{false}, _producerDone{false};
    uint8_t* _generatedData = nullptr;
    bool _success = false, _reported = false;
    bool _utcSet = false, _preserved = false, _readback = false;
    Mode _mode = Mode::fresh;
    uint32_t _runLimitSeconds = _runSeconds;
    bool _usedRefused = false, _fullRefused = false, _layoutUnchanged = false;
    bool _layoutReady = false;
    const char* _failure = "none";
    State _lastReportedState = State::idle;
};
