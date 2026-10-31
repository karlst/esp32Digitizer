// Copyright (C) 2026 Karl Stock
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file FileRecordingBackend.h
 * Reusable session recorder: owned ring, preallocated file pool and readback.
 */
#pragma once
#include "BufferedWriter.h"
#include "RecordingDisk.h"
#include "RecordingSessionConfig.h"
#include "SessionFileHeader.h"
#include "ff.h"
#include <atomic>
#include <string>
#include <vector>

/**
 * Hold incoming data in a ring buffer and write it to a prepared pool of files.
 * One task submits bytes; a separate disk task calls prepForRecording, pump and finishRecording.
 * The application creates these tasks, not this class.
 *
 * prepForRecording waits for disk setup and returns true only when ready. copyToRingBuffer copies
 * bytes into RAM without waiting for disk writes. The ring contains sample data
 * only; this backend writes the file headers separately.
 *
 * Count rejected input and store the final loss total in retained headers after
 * submission stops. Individual gaps are not recorded, so files from a session
 * with loss cannot give the exact time of every sample. This class does not
 * depend on an ADC, board pins, Feather or user interface.
 */
class FileRecordingBackend : private ByteSink
{
public:
    static constexpr uint32_t _maxSubmission = 65520;
    /* **
     * FileRecordingBackend - Construct the backend without allocating memory or
     * accessing the card. Call initializeMemory and prepForRecording before submitting data.
     */
    FileRecordingBackend() = default;
    ~FileRecordingBackend();
    bool initializeMemory(uint8_t* (*allocateRing)(uint32_t), void (*releaseRing)(uint8_t*),
        uint64_t (*clockUs)(), uint64_t (*utcSeconds)());
    bool prepForRecording(RecordingDisk& recordingDisk, const RecordingSessionConfig& recordingSessionConfig);
    bool preallocate(RecordingDisk& recordingDisk, const RecordingSessionConfig& recordingSessionConfig);

    /* **
     * setProgressReporter - Save an optional function for reporting preparation
     * progress. Call before preparation. The disk task calls this function with
     * temporary text; a reporter that keeps the message must copy it.
     */
    void setProgressReporter(void (*callback)(void*, const char*), void* context)
    {
        _progressReporter = callback;
        _progressContext = context;
    }

    uint32_t copyToRingBuffer(const uint8_t* data, uint32_t length);
    void skipInput(uint64_t bytes);
    bool pump(bool drain = false);
    bool finishRecording();
    bool verify(bool (*checkPayload)(const uint8_t*, uint32_t, uint64_t));
    bool verify(bool (*checkPayload)(void*, const uint8_t*, uint32_t, uint64_t), void* context);
    bool getFailed() const;
    bool getFull() const;
    uint32_t getQueuedBytes() const;

    /**
     * Recording totals maintained by the disk task. Other tasks need a copy
     * protected against simultaneous updates; they must not read these fields while
     * the disk task changes them.
     */
    struct Statistics
    {
        uint64_t _session = 0, _written = 0, _writeUs = 0, _rolloverUs = 0;
        uint64_t _lostBytes = 0; // Final loss total, set by finishRecording after sample submission stops.
        uint32_t _fileNumber = 0, _files = 0, _rollovers = 0, _reuses = 0;
        BufferedWriter::Statistics _buffer;
    };

    Statistics getStatistics();
    std::vector<uint64_t> getFileCapacities() const;
    const char* getError() const;

private:
    /* **
     * reportProgress - Report one preparation step if a callback was supplied.
     * This is called before acquisition, not while writing incoming samples.
     */
    void reportProgress(const char* text) const
    {
        if (_progressReporter)
        {
            _progressReporter(_progressContext, text);
        }
    }

    void (*_progressReporter)(void*, const char*) = nullptr;
    void* _progressContext = nullptr;

    /**
     * Keep the path and current header for one selected recording file.
     * _writtenThisSession distinguishes files changed by this run from files that
     * must be preserved.
     */
    struct Slot
    {
        std::string _path;
        SessionFileHeader _sessionFileHeader;
        bool _writtenThisSession = false;
    };

    bool scanHeaders(std::vector<Slot>& slots, uint32_t& nextFile, uint64_t& nextSession, uint32_t& entries);
    void resetSession(const RecordingSessionConfig& config);
    bool validateConfig() const;
    bool prepareDisk(RecordingDisk& recordingDisk);
    bool selectSlots(const std::vector<Slot>& existing);
    bool prepareFiles(RecordingDisk& recordingDisk);
    bool startRecordingSession();
    bool prepareSession(RecordingDisk& recordingDisk, const RecordingSessionConfig& config, bool startRecording);
    bool allocate(uint32_t first, uint64_t capacity);
    bool openSlot();
    bool closeSlot();
    bool advance();
    bool saveHeader();
    bool finalizeLossTotals();
    bool checkOperation(FRESULT result, const char* operation);
    size_t write(const uint8_t* data, size_t length) override;
    bool flush() override;

    BufferedWriter _bufferedWriter;
    RecordingSessionConfig _recordingSessionConfig;
    std::vector<Slot> _slots;
    std::string _directory;
    FIL _file = {};
    uint8_t* _scratchMemory = nullptr;
    uint64_t (*_clockUs)() = nullptr;
    uint64_t (*_utcSeconds)() = nullptr;
    std::atomic<bool> _failed{false}, _full{false};
    bool _active = false, _opened = false, _memoryReady = false;
    const char* _error = "none";
    uint64_t _session = 0, _sequence = 0, _written = 0, _sessionCapacity = 0;

    // Only the submitting task updates these; the disk task reads them after submission stops.
    uint64_t _inputPosition = 0, _accepted = 0, _lostInput = 0;
    uint32_t _iSlot = 0;
    Statistics _statistics;
};
