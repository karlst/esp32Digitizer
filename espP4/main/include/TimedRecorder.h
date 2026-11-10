// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// Board adapter for interval staging, version-1 formatting and buffered SD I/O.
#pragma once
#include "TimedIntervalBuffer.h"
#include "TimedBufferedSink.h"
#include "TimedFatStorage.h"
#include "P4RecordingDisk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * Decoder/Core 1 submits ordinary ADC words. Timing/Core 0 submits boundaries.
 * A Core 0 formatting task copies completed intervals from 6-MiB staging into
 * the reusable 16-MiB writer. A separate Core 0 disk task pumps 128-KiB chunks
 * and owns every filesystem call. Formatting can proceed while SD waits for
 * hardware. Decoder and formatter submission never wait for disk I/O.
 *
 * The caller supplies UTC for the first retained PPS, not the preparation time.
 * For the bench test this is explicitly synthetic. initialize allocates once;
 * prepare creates a fresh file pool preserving older sessions; getReady confirms
 * preparation. finish is requested only after both submitting callers stop.
 * Complete SD readback verifies every submitted byte using a CRC checksum.
 */
class TimedRecorder : private TimedFileSink
{
public:
    TimedRecorder();
    bool initialize();
    bool prepare(uint32_t run, uint64_t utcFirst, const TimedRecordingConfig& config);
    bool getReady() const;
    bool getFailed() const;
    bool submit(const uint32_t* words, size_t count);
    bool addBoundary(const TimedBoundary& boundary);
    void finish(bool clean);
private:
    enum class Phase { idle, preparing, running, complete };
    static void taskEntry(void* context);
    static void formatEntry(void* context);
    void work();
    void format();
    void complete();
    void recordBacklog(bool final = false);
    static bool check(void* context, uint32_t iSlot, uint64_t offset, const uint8_t* bytes, uint32_t count);
    bool beginFile(uint32_t iSlot, uint64_t utcSample) override;
    bool submit(const uint8_t* bytes, size_t count) override;
    bool endFile() override;
    P4RecordingDisk _p4RecordingDisk;
    TimedFatStorage _timedFatStorage;
    TimedBufferedSink _timedBufferedSink;
    TimedRecordingStream _timedRecordingStream;
    TimedIntervalBuffer _timedIntervalBuffer;
    TimedRecordingConfig _config;
    TaskHandle_t _task = nullptr, _formatTask = nullptr;
    std::atomic<Phase> _phase{Phase::idle};
    std::atomic<bool> _failed{false}, _finishRequested{false};
    // Formatter publishes completion after its last ring submission. Disk task
    // acquires this flag before reading formatter statistics or draining/closing.
    std::atomic<bool> _formatFinished{false};
    bool _formatPassed = false;
    bool _clean = false;
    uint8_t* _staging = nullptr;
    static constexpr uint32_t _stagingSamples = 2097152;
    uint8_t _packed[512 * 3];
    uint32_t _run = 0;
    uint64_t _utcFirst = 0;
    // Formatter owns crc/files/bytes. Disk owns readCrc/readBytes and reads the
    // formatter's fields only after formatFinished. No 64-bit atomic is needed.
    uint32_t _crc = 0, _readCrc = 0, _files = 0;
    uint64_t _bytes = 0, _readBytes = 0;
    // Each task owns its own elapsed total, excluding deliberate sleeps/readback.
    uint64_t _formatUs = 0, _diskUs = 0;
    uint32_t _workCycles = 0;
    /**
     * Disk-task snapshots of queue occupancy and accepted bytes. _elapsedUs
     * measures time since prepared storage, not GPS time. A blocked write delays
     * snapshots, so gaps between these rows are not necessarily sample loss.
     * _pendingBytes belongs to the adapter's transfer buffer, outside the ring.
     */
    struct BacklogSample
    {
        uint64_t _elapsedUs = 0, _writtenBytes = 0;
        uint32_t _queuedBytes = 0, _peakBytes = 0, _overflows = 0, _pendingBytes = 0;
    };
    // Five-minute bench run plus shutdown headroom. No allocation or printing
    // during capture. Only the disk task records and later reports these rows.
    BacklogSample _backlogSamples[320];
    unsigned _backlogCount = 0;
    uint64_t _backlogStartedUs = 0, _backlogLastUs = 0;
};
