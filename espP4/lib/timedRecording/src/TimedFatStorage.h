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
 * The 16-KiB internal transfer buffer retains partial caller spans. During
 * recording, only full buffers reach FatFs; close writes the unpadded final
 * tail. write returns copied/owned bytes, not confirmed SD bytes. flush/close
 * and getFailed must be checked, including errors after a full return count.
 */
class TimedFatStorage : public TimedSlotStorage
{
public:
    enum class Operation { open, copy, write, sync, close, count };
    /** Elapsed microseconds for one operation, owned only by the disk task. */
    struct OperationTiming
    {
        uint64_t _calls = 0, _totalUs = 0, _maxUs = 0;
        uint64_t _offset = 0, _atUs = 0;
        uint32_t _requested = 0, _actual = 0, _result = 0, _failures = 0;
    };
    /** One prepared file's operations; excludes preallocation and readback. */
    struct SlotTiming
    {
        OperationTiming _operations[unsigned(Operation::count)];
    };
    void setTimingClock(uint64_t (*clockUs)());
    const std::vector<SlotTiming>& getTimings() const;
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
    uint32_t getPendingBytes() const;
    bool readback(bool (*check)(void*, uint32_t, uint64_t, const uint8_t*, uint32_t), void* context);
    const char* getDirectory() const;
    const char* getError() const;
private:
    bool writePending();
    void recordTiming(uint32_t iSlot, Operation operation, uint64_t began,
        uint64_t offset, uint32_t requested, uint32_t actual, uint32_t result);
    uint64_t (*_clockUs)() = nullptr;
    uint64_t _timingOrigin = 0;
    std::vector<SlotTiming> _timings;
    bool allocateSlots(uint32_t fileCount);
    FIL _file = {};
    std::vector<std::string> _paths;
    std::vector<uint64_t> _validBytes;
    std::string _directory;
    uint8_t* _scratch = nullptr;
    void (*_release)(uint8_t*) = nullptr;
    static constexpr uint32_t _scratchBytes = 16384;
    // Copied bytes owned by this adapter but not yet accepted by FatFs.
    uint32_t _scratchUsed = 0;
    uint64_t _capacity = 0, _position = 0;
    uint32_t _iSlot = 0;
    bool _opened = false, _failed = false;
    const char* _error = "none";
};
