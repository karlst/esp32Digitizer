// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// prepare allocates without erasing. open/write/close reuse those slots. readback
// follows acquisition/flush, never interleaves with writing from another task.
#include "TimedFatStorage.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

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
 * openSlot - Open only a preallocated session path and rewind to byte zero.
 * Reuse does not resize or reallocate the file. A failed seek still closes the
 * FatFs handle so a later error report does not leak an open file.
 */
bool TimedFatStorage::openSlot(uint32_t iSlot)
{
    _error = "opening timed slot";
    bool retVal = !_opened && iSlot < _paths.size() &&
        f_open(&_file, _paths[iSlot].c_str(), FA_WRITE) == FR_OK;
    if (retVal)
    {
        retVal = f_lseek(&_file, 0) == FR_OK;
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
    return retVal;
}

/* **
 * write - Copy each ring span through internal scratch, then wait for f_write
 * before reusing it. Individual disk calls are at most 16 KiB, matching the
 * existing writer. Return the exact accepted prefix on an error; never retry.
 */
size_t TimedFatStorage::write(const uint8_t* data, size_t bytes)
{
    size_t retVal = 0;
    bool writing = _opened && data && bytes <= _capacity - _position;
    _failed = _failed || !writing;
    _error = "writing timed slot";
    while (writing && retVal < bytes)
    {
        const UINT amount = static_cast<UINT>(std::min<size_t>(_scratchBytes, bytes - retVal));
        std::memcpy(_scratch, data + retVal, amount);
        UINT actual = 0;
        const FRESULT result = f_write(&_file, _scratch, amount, &actual);
        const UINT accepted = std::min(actual, amount);
        retVal += accepted;
        _position += accepted;
        writing = result == FR_OK && actual == amount;
        // Preserve the actual count even if an error accompanies a full write.
        // TimedBufferedSink also checks getFailed; no error becomes a success.
        _failed = _failed || !writing;
    }
    return retVal;
}

/* **
 * flush - Ask FatFs to synchronize the open file, or succeed if already closed.
 */
bool TimedFatStorage::flush()
{
    _error = "synchronizing timed slot";
    return !_opened || f_sync(&_file) == FR_OK;
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
        const bool closed = f_close(&_file) == FR_OK;
        retVal = synced && closed;
        _opened = false;
        if (retVal)
        {
            _validBytes[_iSlot] = _position;
        }
    }
    return retVal;
}

/* **
 * readback - After writing stops, visit retained bytes of each used slot in slot
 * order, not chronological order after circular reuse. check receives slot,
 * per-file offset and a borrowed scratch span. It may validate CRC or contents
 * and must copy anything it retains. Unused allocated tails are not inspected.
 */
bool TimedFatStorage::readback(bool (*check)(void*, uint32_t, uint64_t, const uint8_t*, uint32_t), void* context)
{
    bool retVal = !_opened && check;
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
