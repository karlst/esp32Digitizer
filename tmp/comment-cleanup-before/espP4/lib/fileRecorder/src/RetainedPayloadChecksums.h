// Copyright (C) 2026 Karl Stock
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file RetainedPayloadChecksums.h
 * Expected checksums of accepted bytes surviving circular file reuse.
 */
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

/**
 * Calculate expected checksums from the data accepted into the ring, then
 * compare them with data read back from SD. Keep one checksum for each file;
 * reset it whenever that file is reused. This lets verification check the data
 * still retained after circular recording.
 *
 * prepare allocates the records before acquisition. The task submitting samples
 * calls accept without allocating memory or accessing disk. After that task
 * stops, the disk task calls beginReadback, check and getVerified. These
 * operations must not overlap; this class has no internal lock.
 */
class RetainedPayloadChecksums
{
public:
    using Crc = uint32_t (*)(uint32_t, const uint8_t*, uint32_t);
    bool prepare(const std::vector<uint64_t>& capacities, Crc crc);
    void accept(const uint8_t* data, uint32_t bytes);
    uint64_t beginReadback();
    bool check(const uint8_t* data, uint32_t bytes, uint64_t offset);
    bool getVerified() const;

private:
    /**
     * Checksum and byte range for the most recent data assigned to one file.
     * Capacity, offset and length count sample-data bytes, excluding the file
     * header.
     */
    struct Entry
    {
        uint64_t _capacity = 0, _offset = 0, _length = 0;
        uint32_t _crc = 0;
    };

    std::vector<Entry> _entries;
    Crc _crc = nullptr;
    size_t _iEntry = 0, _iReadEntry = 0;
    uint64_t _accepted = 0, _readAt = 0, _readInFile = 0;
    uint32_t _readCrc = 0;
    bool _valid = false;
};
