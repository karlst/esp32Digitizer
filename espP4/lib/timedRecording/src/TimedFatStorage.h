// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// FatFs implementation of a timed recording's prepared file pool.
#pragma once
#include "TimedSlotStorage.h"
#include "RecordingDisk.h"
#include "ff.h"
#include <string>
#include <vector>

/**
 * Disk-task-only file operations. Preparation mounts without formatting and
 * creates a new numbered directory under TIMED. Every slot is preallocated
 * before acquisition; circular recording reuses only those paths. Unrelated
 * files and older sessions are never overwritten or deleted.
 *
 * No format logic is embedded here: headers, tBlocks, samples and end markers
 * arrive as ordinary bytes from TimedBufferedSink. Files retain allocated tail
 * space; the format's end marker determines valid content. No header patch or
 * truncation is performed on close. readback visits only bytes written this run.
 */
class TimedFatStorage : public TimedSlotStorage
{
public:
    /* **
     * TimedFatStorage - Construct without allocating scratch or accessing files.
     */
    TimedFatStorage() = default;
    ~TimedFatStorage();
    TimedFatStorage(const TimedFatStorage&) = delete;
    TimedFatStorage& operator=(const TimedFatStorage&) = delete;
    bool initializeMemory(uint8_t* (*allocate)(uint32_t), void (*release)(uint8_t*));
    bool prepare(RecordingDisk& recordingDisk, uint32_t fileCount, uint64_t capacity);
    bool openSlot(uint32_t iSlot) override;
    size_t write(const uint8_t* data, size_t bytes) override;
    bool flush() override;
    bool closeSlot() override;
    bool getFailed() const override;
    bool readback(bool (*check)(void*, uint32_t, uint64_t, const uint8_t*, uint32_t), void* context);
    const char* getDirectory() const;
    const char* getError() const;
private:
    bool allocateSlots(uint32_t fileCount);
    FIL _file = {};
    std::vector<std::string> _paths;
    std::vector<uint64_t> _validBytes;
    std::string _directory;
    uint8_t* _scratch = nullptr;
    void (*_release)(uint8_t*) = nullptr;
    static constexpr uint32_t _scratchBytes = 16384;
    uint64_t _capacity = 0, _position = 0;
    uint32_t _iSlot = 0;
    bool _opened = false, _failed = false;
    const char* _error = "none";
};
