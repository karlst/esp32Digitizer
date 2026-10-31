// Copyright (C) 2026 Karl Stock
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file RetainedPayloadChecksums.cpp
 * Follow the backend's byte-count boundaries without copying sample data.
 */
#include "RetainedPayloadChecksums.h"
#include <algorithm>

/* **
 * prepare - Set up one checksum record per file before recording starts.
 * capacities lists sample-data bytes per file in writing order. crc is the
 * caller's function for updating a checksum. Return false if the function or
 * capacities are missing, or a capacity is zero.
 */
bool RetainedPayloadChecksums::prepare(const std::vector<uint64_t>& capacities, Crc crc)
{
    _entries.clear();
    _crc = crc;
    _iEntry = 0;
    _accepted = 0;
    bool retVal = crc && !capacities.empty();
    for (uint64_t capacity : capacities)
    {
        retVal = retVal && capacity != 0;
        Entry entry;
        entry._capacity = capacity;
        _entries.push_back(entry);
    }
    _valid = retVal;
    return retVal;
}

/* **
 * accept - Update checksums from the bytes actually accepted into the ring
 * buffer. The sample-submission task calls this after copyToRingBuffer. Split batches at
 * file boundaries and replace a file's old checksum when that file is reused.
 * This allocates no memory and writes nothing to disk.
 */
void RetainedPayloadChecksums::accept(const uint8_t* data, uint32_t bytes)
{
    uint32_t used = 0;
    while (_valid && used < bytes)
    {
        if (_entries[_iEntry]._length == _entries[_iEntry]._capacity)
        {
            _iEntry = (_iEntry + 1) % _entries.size();
            auto& next = _entries[_iEntry];
            next._offset = _accepted;
            next._length = 0;
            next._crc = 0;
        }
        auto& entry = _entries[_iEntry];
        const uint32_t amount = static_cast<uint32_t>(std::min<uint64_t>(bytes - used, entry._capacity - entry._length));
        entry._crc = _crc(entry._crc, data + used, amount);
        entry._length += amount;
        _accepted += amount;
        used += amount;
    }
}

/* **
 * beginReadback - Reset checksum comparison after all sample submissions have
 * stopped. Return the original session byte offset of the oldest data still
 * retained. The disk task uses that offset to start checking the remaining files
 * in recording order.
 */
uint64_t RetainedPayloadChecksums::beginReadback()
{
    uint64_t retVal = _accepted;
    _iReadEntry = 0;
    _readInFile = 0;
    _readCrc = 0;
    for (size_t iEntry = 0; iEntry < _entries.size(); ++iEntry)
    {
        if (_entries[iEntry]._length && _entries[iEntry]._offset < retVal)
        {
            retVal = _entries[iEntry]._offset;
            _iReadEntry = iEntry;
        }
    }
    _readAt = retVal;
    _valid = _valid && retVal < _accepted;
    return retVal;
}

/* **
 * check - Compare bytes read from disk with the checksums calculated during
 * submission. offset is the original byte position in the session, not the
 * position within a file. Small or split reads are allowed.
 *
 * Return false for missing, repeated, out-of-order or changed data. Once a check
 * fails, later calls also fail. Only complete file contents can be compared
 * against that file's checksum.
 */
bool RetainedPayloadChecksums::check(const uint8_t* data, uint32_t bytes, uint64_t offset)
{
    _valid = _valid && offset == _readAt && bytes <= _accepted - _readAt;
    uint32_t used = 0;
    while (_valid && used < bytes)
    {
        const auto& entry = _entries[_iReadEntry];
        _valid = entry._length && _readAt == entry._offset + _readInFile;
        if (_valid)
        {
            const uint32_t amount = static_cast<uint32_t>(std::min<uint64_t>(bytes - used, entry._length - _readInFile));
            _readCrc = _crc(_readCrc, data + used, amount);
            _readInFile += amount;
            _readAt += amount;
            used += amount;
            if (_readInFile == entry._length)
            {
                _valid = _readCrc == entry._crc;
                _iReadEntry = (_iReadEntry + 1) % _entries.size();
                _readInFile = 0;
                _readCrc = 0;
            }
        }
    }
    return _valid;
}

/* **
 * getVerified - Return true only after all retained bytes have been read in
 * order and every retained file checksum matched. This says nothing about
 * earlier files that were overwritten.
 */
bool RetainedPayloadChecksums::getVerified() const
{
    return _valid && _readAt == _accepted && _readInFile == 0;
}
