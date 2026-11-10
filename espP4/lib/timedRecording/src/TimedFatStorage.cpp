// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Disk-task adapter for FatFs (the SD filesystem library). Read prepare for file
 * allocation, openSlot/write/writePending/closeSlot for storage, and readback for
 * verification after shutdown. No other task may call these methods concurrently.
 *
 * A 16-KiB internal buffer owns copied bytes until f_write accepts them. It
 * accumulates a full transfer across calls from the 128-KiB write ring, always
 * relative to this file's byte zero. Closing writes the exact short tail without
 * padding. This preserves alignment after rotation even when the file length
 * is not a sector multiple. _position includes the RAM tail; FatFs has accepted
 * _position-_scratchUsed bytes. f_sync/close must still succeed before completion.
 * The separate SD DMA alignment requirement is supplied by the board allocator.
 */
#include "TimedFatStorage.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <ctime>

/* **
 * setTimingClock - Before preparation, supply an optional monotonic microsecond
 * clock. No ESP-specific dependency is required by this reusable adapter.
 * The caller must not change the clock while disk work is active.
 */
void TimedFatStorage::setTimingClock(uint64_t (*clockUs)())
{
    _clockUs = clockUs;
}

/* **
 * getTimings - Borrow per-file measurements after writing has stopped, on the
 * disk task. Entries remain valid until the next prepare. Durations include
 * preemption and hardware waits, not just time executing this task's code.
 */
const std::vector<TimedFatStorage::SlotTiming>& TimedFatStorage::getTimings() const
{
    return _timings;
}

/* **
 * recordTiming - Disk task updates fixed, previously allocated counters without
 * console output. Keep location, size and FatFs result for the longest call.
 * atUs is its start relative to the first recording-file open, excluding pool
 * preparation. A short write counts as failure even when FatFs returned FR_OK.
 */
void TimedFatStorage::recordTiming(uint32_t iSlot, Operation operation, uint64_t began,
    uint64_t offset, uint32_t requested, uint32_t actual, uint32_t result)
{
    if (_clockUs && iSlot < _timings.size())
    {
        auto& timing = _timings[iSlot]._operations[unsigned(operation)];
        const uint64_t elapsed = _clockUs() - began;
        if (timing._calls == 0 || elapsed > timing._maxUs)
        {
            timing._maxUs = elapsed;
            timing._offset = offset;
            timing._atUs = began - _timingOrigin;
            timing._requested = requested;
            timing._actual = actual;
            timing._result = result;
        }
        ++timing._calls;
        timing._totalUs += elapsed;
        timing._failures += result != FR_OK || actual != requested;
    }
}

/* **
 * ~TimedFatStorage - Free the owned DMA-safe scratch allocation. The caller
 * must finish disk work before destruction; no implicit file write is attempted.
 */
TimedFatStorage::~TimedFatStorage()
{
    if (_scratch && _release)
    {
        _release(_scratch);
    }
}

/* **
 * initializeMemory - Allocate 16 KiB scratch outside the disk task's stack.
 * On P4 the supplied allocator must return internal, byte-addressable RAM safe
 * for the SD driver. Repeated calls keep the existing allocation.
 */
bool TimedFatStorage::initializeMemory(uint8_t* (*allocate)(uint32_t), void (*release)(uint8_t*))
{
    if (!_scratch && allocate && release)
    {
        _scratch = allocate(_scratchBytes);
        _release = release;
    }
    return _scratch != nullptr;
}

/* **
 * prepare - Mount the existing card, choose a fresh session directory, and
 * allocate fileCount slots of capacity bytes, including format headers/markers.
 * Caller supplies headroom for PPS/sample-clock drift. Existing names are never
 * reused here. Failure may leave new unused files, but deletes nothing.
 */
bool TimedFatStorage::prepare(RecordingDisk& recordingDisk, uint32_t fileCount, uint64_t capacity)
{
    _error = "invalid timed storage settings";
    bool retVal = !_opened && _scratch && fileCount > 0 && fileCount <= 4096 &&
        capacity >= 304 && capacity <= std::numeric_limits<FSIZE_t>::max();
    if (retVal)
    {
        _error = "mounting existing card";
        retVal = recordingDisk.prepare(false);
    }
    if (retVal)
    {
        _paths.clear();
        _validBytes.clear();

        // Allocate diagnostics before acquisition; writing never grows them.
        _timings.assign(_clockUs ? fileCount : 0, SlotTiming{});
        _timingOrigin = 0;
        _capacity = capacity;
        const std::string parent = std::string(recordingDisk.getDrive()) + "/TIMED";
        const FRESULT parentResult = f_mkdir(parent.c_str());
        _error = "creating timed directory";
        retVal = parentResult == FR_OK || parentResult == FR_EXIST;
        bool created = false;
        for (uint32_t session = 1; retVal && !created && session <= 999999; ++session)
        {
            char suffix[20];
            std::snprintf(suffix, sizeof(suffix), "/S%06lu", (unsigned long)session);
            _directory = parent + suffix;
            const FRESULT result = f_mkdir(_directory.c_str());
            created = result == FR_OK;
            retVal = created || result == FR_EXIST;
        }
        retVal = retVal && created && allocateSlots(fileCount);
    }
    if (retVal)
    {
        _error = "none";
        _failed = false;
        _scratchUsed = 0;
    }
    return retVal;
}

/* **
 * allocateSlots - Reserve each new file with FatFs f_expand before data arrive.
 * FA_CREATE_NEW refuses collisions. Files are left at their reserved lengths;
 * normal completion later uses the stream's zero marker rather than truncation.
 */
bool TimedFatStorage::allocateSlots(uint32_t fileCount)
{
    bool retVal = true;
    for (uint32_t iSlot = 0; retVal && iSlot < fileCount; ++iSlot)
    {
        char name[20];
        std::snprintf(name, sizeof(name), "/F%04lu.DG", (unsigned long)iSlot);
        const std::string path = _directory + name;
        _error = "creating timed slot";
        retVal = f_open(&_file, path.c_str(), FA_WRITE | FA_CREATE_NEW) == FR_OK;
        if (retVal)
        {
            _error = "allocating timed slot";
            retVal = f_expand(&_file, static_cast<FSIZE_t>(_capacity), 1) == FR_OK;
            const bool closed = f_close(&_file) == FR_OK;
            retVal = retVal && closed;
            if (retVal)
            {
                _paths.push_back(path);
                _validBytes.push_back(0);
            }
        }
    }
    return retVal;
}

/* **
 * renameSlot - Disk task names a closed preallocated file for its first saved
 * sample's whole UTC second. Rename preserves its allocation and byte offsets.
 * Refuse name collisions; never delete a destination to make the rename work.
 * Update the slot path only after success so reuse and readback find that file.
 * Measure f_rename separately: its directory writes may wait on the card.
 */
bool TimedFatStorage::renameSlot(uint32_t iSlot, uint64_t utcSample)
{
    // Restrict the name to four-digit years and a representable library time.
    bool retVal = utcSample <= 253402300799ULL &&
        utcSample <= uint64_t(std::numeric_limits<time_t>::max());
    tm utc = {};
    if (retVal)
    {
        const time_t seconds = static_cast<time_t>(utcSample);
#ifdef _WIN32
        retVal = gmtime_s(&utc, &seconds) == 0;
#else
        retVal = gmtime_r(&seconds, &utc) != nullptr;
#endif
    }
    char name[32] = {};
    retVal = retVal && std::strftime(name, sizeof(name), "/LXD%Y%m%d-%H%M%S.bin", &utc) == 23;
    _error = "naming timed slot";
    if (retVal)
    {
        const std::string path = _directory + name;
        if (path != _paths[iSlot])
        {
            _error = "renaming timed slot";
            const uint64_t began = _clockUs ? _clockUs() : 0;
            const FRESULT result = f_rename(_paths[iSlot].c_str(), path.c_str());
            recordTiming(iSlot, Operation::rename, began, 0, 0, 0, result);
            retVal = result == FR_OK;
            if (retVal)
            {
                _paths[iSlot] = path;
            }
        }
    }
    _failed = _failed || !retVal;
    return retVal;
}

/* **
 * openSlot - Rename a closed preallocated slot, open it and rewind to byte zero.
 * utcSample is the first saved analog sample's UTC second, calculated by the
 * formatter and copied through the file-boundary queue. No local timezone is used.
 * Reuse does not resize or reallocate the file. A failed seek still closes the
 * FatFs handle so a later error report does not leak an open file.
 */
bool TimedFatStorage::openSlot(uint32_t iSlot, uint64_t utcSample)
{
    uint64_t began = _clockUs ? _clockUs() : 0;
    if (_timingOrigin == 0)
    {
        _timingOrigin = began;
    }
    _error = "opening timed slot";
    FRESULT result = FR_INVALID_PARAMETER;
    if (!_opened && !_failed && _scratchUsed == 0 && iSlot < _paths.size())
    {
        if (renameSlot(iSlot, utcSample))
        {
            _error = "opening timed slot";
            began = _clockUs ? _clockUs() : 0;
            result = f_open(&_file, _paths[iSlot].c_str(), FA_WRITE);
        }
    }
    bool retVal = result == FR_OK;
    if (retVal)
    {
        result = f_lseek(&_file, 0);
        retVal = result == FR_OK;
        if (retVal)
        {
            _iSlot = iSlot;
            _position = 0;
            _validBytes[iSlot] = 0;
            _opened = true;
        }
        else
        {
            f_close(&_file);
        }
    }
    recordTiming(iSlot, Operation::open, began, 0, 0, 0, result);
    return retVal;
}

/* **
 * getPath - After disk work stops, obtain the current name of a retained slot
 * for reporting or verification. Invalid slot indices return an empty string.
 */
const char* TimedFatStorage::getPath(uint32_t iSlot) const
{
    return iSlot < _paths.size() ? _paths[iSlot].c_str() : "";
}

/* **
 * write - Copy ring bytes into owned internal RAM, retaining a partial 16-KiB
 * block across calls. This makes full writes follow file offsets, independent
 * of ring chunks split at rotation. Return bytes copied into our ownership;
 * this is not confirmation of SD storage. Full blocks are written immediately;
 * flush/close writes the final short block. Never retry after a disk failure.
 */
size_t TimedFatStorage::write(const uint8_t* data, size_t bytes)
{
    size_t retVal = 0;
    bool writing = !_failed && _opened && data && bytes <= _capacity - _position;
    _failed = _failed || !writing;
    _error = "writing timed slot";
    while (writing && retVal < bytes)
    {
        const UINT amount = static_cast<UINT>(std::min<size_t>(_scratchBytes - _scratchUsed, bytes - retVal));

        // memcpy transfers ownership from the caller's ring span to _scratch.
        // Example: a 100-byte submission followed by 16284 bytes fills one
        // 16384-byte file transfer. A short file tail is not carried into the
        // next file. Measure copying separately from the f_write call below.
        uint64_t began = _clockUs ? _clockUs() : 0;
        std::memcpy(_scratch + _scratchUsed, data + retVal, amount);
        recordTiming(_iSlot, Operation::copy, began, _position, amount, amount, FR_OK);
        _scratchUsed += amount;
        retVal += amount;
        _position += amount;
        if (_scratchUsed == _scratchBytes)
        {
            writing = writePending();
        }
    }
    return retVal;
}

/* **
 * writePending - Disk task writes the owned block, starting at its file offset.
 * Only flush/close calls this with a short block. On a short or failed write,
 * latch failure and retain the unwritten byte count for reporting. The scratch
 * contents are never retried or reused after failure; prepare starts a new run.
 */
bool TimedFatStorage::writePending()
{
    bool retVal = !_failed;
    if (retVal && _scratchUsed != 0)
    {
        const UINT requested = _scratchUsed;
        UINT actual = 0;
        const uint64_t began = _clockUs ? _clockUs() : 0;
        const FRESULT result = f_write(&_file, _scratch, requested, &actual);

        // f_write is the actual filesystem submission. A short count is a
        // failure even with FR_OK. Retain the unaccepted count for reporting;
        // do not retry the unchanged buffer, which could duplicate its prefix.
        recordTiming(_iSlot, Operation::write, began, _position - _scratchUsed, requested, actual, result);
        _scratchUsed -= std::min(actual, requested);
        retVal = result == FR_OK && actual == requested;
        _failed = !retVal;
    }
    return retVal;
}

/* **
 * flush - Write the retained tail, then synchronize FatFs. A failed write is
 * never retried, but sync is still attempted before close. Failure stays latched.
 */
bool TimedFatStorage::flush()
{
    _error = "synchronizing timed slot";
    bool retVal = !_failed;
    if (_opened)
    {
        const bool written = writePending();
        const uint64_t began = _clockUs ? _clockUs() : 0;
        const FRESULT result = f_sync(&_file);
        recordTiming(_iSlot, Operation::sync, began, _position, 0, 0, result);
        retVal = written && result == FR_OK;
        _failed = _failed || !retVal;
    }
    return retVal;
}

/* **
 * closeSlot - Synchronize and close even when synchronization fails. Retain the
 * exact written length for readback only on success; do not patch the header.
 */
bool TimedFatStorage::closeSlot()
{
    bool retVal = _opened;
    if (_opened)
    {
        const bool synced = flush();
        const uint64_t began = _clockUs ? _clockUs() : 0;
        const FRESULT result = f_close(&_file);
        recordTiming(_iSlot, Operation::close, began, _position, 0, 0, result);
        const bool closed = result == FR_OK;
        retVal = synced && closed;
        _failed = _failed || !retVal;
        _opened = false;
        if (retVal)
        {
            _validBytes[_iSlot] = _position;
        }
    }
    return retVal;
}

/* **
 * readback - After writing stops, f_read retrieves retained bytes in slot-index
 * order, not chronological order after circular reuse. check receives slot,
 * per-file offset and a borrowed scratch span. It may validate CRC or contents
 * and must copy anything it retains. Unused allocated tails are not inspected.
 */
bool TimedFatStorage::readback(bool (*check)(void*, uint32_t, uint64_t, const uint8_t*, uint32_t), void* context)
{
    bool retVal = !_opened && !_failed && _scratchUsed == 0 && check;
    _error = "reading timed slots";
    for (uint32_t iSlot = 0; retVal && iSlot < _paths.size(); ++iSlot)
    {
        if (_validBytes[iSlot])
        {
            retVal = f_open(&_file, _paths[iSlot].c_str(), FA_READ) == FR_OK;
            if (retVal)
            {
                uint64_t offset = 0;
                while (retVal && offset < _validBytes[iSlot])
                {
                    const UINT amount = static_cast<UINT>(std::min<uint64_t>(_scratchBytes, _validBytes[iSlot] - offset));
                    UINT actual = 0;
                    retVal = f_read(&_file, _scratch, amount, &actual) == FR_OK && actual == amount &&
                        check(context, iSlot, offset, _scratch, actual);
                    offset += actual;
                }
                const bool closed = f_close(&_file) == FR_OK;
                retVal = retVal && closed;
            }
        }
    }
    return retVal;
}

/* **
 * getDirectory - Borrow the session directory name until the next preparation.
 */
const char* TimedFatStorage::getDirectory() const
{
    return _directory.c_str();
}

/* **
 * getError - Return the most recent operation label for a failed-call report.
 */
const char* TimedFatStorage::getError() const
{
    return _error;
}

/* **
 * getFailed - Disk task checks a latched write error even after a full count.
 */
bool TimedFatStorage::getFailed() const
{
    return _failed;
}

/* **
 * getPendingBytes - Disk-task-only count of owned bytes not accepted by FatFs.
 * Add this to ring unsaved bytes after failure; they have different owners and
 * must not be lost from the report just because copying into scratch succeeded.
 */
uint32_t TimedFatStorage::getPendingBytes() const
{
    return _scratchUsed;
}
