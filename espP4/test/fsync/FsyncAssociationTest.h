/* Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
 * Timing diagnostic services supplied to the real recording application.
 */
#pragma once
#include <atomic>
#include "CaptureDestination.h"
#include "RecordingObserver.h"
#include "RecordingConsoleCommands.h"
#include "FrameDecoder.h"
#include "FsyncAssociationAnalysis.h"
#include "driver/mcpwm_cap.h"
#include "driver/pulse_cnt.h"
#include "PulseDelayCapture.h"
#include "FsyncReadTest.h"
#include "FsyncIndexTest.h"
#include "PpsSampleTest.h"

/**
 * Compare hardware-numbered FSYNC captures with the ordinary decoded words.
 * Core 0 owns setup, hardware snapshots and reporting; Core 1 copies words into
 * private RAM and records decoder-duration statistics. Only the submitted-word
 * counter crosses cores while running.
 * finish reads the other Core 1 data only after the application drains decoding.
 * This class performs no disk I/O. FsyncSdTest can also pass each batch to the
 * ordinary SD recorder. PPS and delay modes need an external generator; no
 * on-board pulse generator is used. The external-source Feather controls ADC
 * start/stop through the existing READY/RAMFULL/STOP handshake.
 */
class FsyncAssociationTest : public CaptureDestination,
                             public RecordingObserver,
                             public RecordingConsoleCommands
{
public:
    void initialize(const FrameDecoder& frameDecoder, PulseDelayCapture* pulseDelayCapture = nullptr);
    bool prepare(uint32_t run, const Ads127Config& config) override;
    bool getReady() const override;
    void cancelPreparation() override;
    bool submit(const uint32_t* words, size_t count) override;
    bool getFailed() const override;
    bool getFull() const override;
    void finish(uint32_t run, uint32_t rate, bool clean, uint32_t samples) override;
    bool getInitialFsyncLow() const override;
    bool getDiscardStartupFrame() const override;
    void started(TaskHandle_t control, TaskHandle_t decoder) override;
    void decoded(int64_t waitUs, int64_t decodeUs, uint32_t bytes) override;
    void paused(int64_t elapsedUs) override;
    void poll() override;
    void stopping() override;
    void command(RecordingApplication& application, char key) override;
    bool getBoundary(unsigned iBoundary, TimedBoundary& boundary) const;

private:
    // Optional ten-second read experiment; Core 0 owns mode and statistics.
    FsyncReadTest _fsyncReadTest;
    bool _readTest = false;
    FsyncIndexTest _fsyncIndexTest;
    bool _indexTest = false;
    PpsSampleTest _ppsSampleTest;
    bool _ppsTest = false;
    // Selected while idle: ordinary PPS tests last 60 seconds; timed SD lasts 300.
    uint32_t _ppsSeconds = 60;
    uint32_t getTargetWords() const;
    bool takeSnapshot();
    void stopHardware();
    const FrameDecoder* _frameDecoder = nullptr;
    PulseDelayCapture* _pulseDelayCapture = nullptr;
    mcpwm_cap_timer_handle_t _timer = nullptr;
    mcpwm_cap_channel_handle_t _capture = nullptr;
    pcnt_unit_handle_t _counter = nullptr;
    pcnt_channel_handle_t _counterChannel = nullptr;

    // Core 1 alone appends words. Core 0 sees the atomic submitted total during
    // acquisition, and reads the retained array only after decoder shutdown.
    static constexpr uint32_t _wordCapacity = 65536;
    static constexpr uint32_t _targetWords = 32768;
    uint32_t* _words = nullptr;
    uint32_t _storedWords = 0;
    // Core 1 updates these; Core 0 reports only after decoding has drained.
    uint32_t _decodedBlocks = 0, _pauseCount = 0;
    int64_t _maxWaitUs = 0, _maxDecodeUs = 0, _maxPauseUs = 0;
    int64_t _waitAfterPauseUs = 0;
    bool _afterPause = false;
    uint64_t _decodeTotalUs = 0, _rawTotalBytes = 0, _ramCopyUs = 0;
    std::atomic<uint32_t> _submitted{0};

    // Core 0's occasional observations, not a timestamp for every ADC sample.
    // The short register-read lock prevents local scheduling during a snapshot.
    FsyncAssociationAnalysis::Anchor _anchors[128] = {};
    size_t _anchorCount = 0;
    uint32_t _rejectedSnapshots = 0;
    uint32_t _highSnapshots = 0, _movingSnapshots = 0, _slowSnapshots = 0;
    portMUX_TYPE _snapshotMux = portMUX_INITIALIZER_UNLOCKED;
    int _finalCount = 0;
    // esp_timer microseconds bound run length; they do not timestamp samples.
    int64_t _armedAt = 0;
    bool _running = false;
    bool _ready = false;
    bool _failed = false;
    bool _idleLow = false;
};
