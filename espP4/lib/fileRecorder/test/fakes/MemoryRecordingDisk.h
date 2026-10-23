// Copyright (C) 2026 Karl Stock
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file MemoryRecordingDisk.h
 * Test storage adapter and fault controls; all file contents stay in PC RAM.
 */
#pragma once
#include "RecordingDisk.h"
#include <map>
#include <string>
#include <vector>
#include <cstdint>

/**
 * Simulate format/mount without sharing board code or touching physical storage.
 */
class MemoryRecordingDisk : public RecordingDisk
{
public:
    bool prepare(bool format) override;
    const char* getDrive() const override;
    static std::map<std::string, std::vector<uint8_t>>& files();
    static void failWrite(bool shortWrite);
    static void failSync();

    /**
     * Requested filesystem writes, used to check batching without measuring time.
     */
    struct Write
    {
        uint64_t _offset = 0;
        uint32_t _length = 0;
    };

    static const std::vector<Write>& getWrites();
    static void clearWrites();
};
