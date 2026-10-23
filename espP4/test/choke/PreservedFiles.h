// Copyright (C) 2026 Karl Stock
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file PreservedFiles.h
 * Test-only before/after checksums of files that addFiles must preserve.
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ff.h"

/**
 * Disk task reads old files before preparation and after timed recording.
 * Entire file contents, including unused reserved tails, are checked. CRC32 detects
 * accidental changes; this is not protection against deliberate checksum collisions.
 * Optional contents=false records names/sizes only and rejects additions on verify.
 * No reads or checksum work take place during the measured acquisition interval.
 */
class PreservedFiles
{
public:
    bool capture(const char* drive, bool contents = true);
    bool verify();
    uint32_t getCount() const;

private:
    /**
     * Baseline file path, physical length and complete-content checksum.
     */
    struct Entry
    {
        std::string _path;
        uint64_t _length = 0;
        uint32_t _checksum = 0;
    };

    bool scan(const std::string& path, unsigned depth, uint8_t* scratch);
    bool read(Entry& entry, uint8_t* scratch);
    std::vector<Entry> _entries;
    std::string _drive;
    bool _contents = true;
    // FatFs embeds a sector cache here (4 KiB in this build). Keep it in the
    // persistent test object, not on the disk task's stack. Only read() uses it.
    FIL _file = {};
};
