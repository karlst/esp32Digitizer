// Copyright (C) 2026 Karl Stock
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file PreservedFiles.cpp
 * Read-only preservation check, independent of the recording backend.
 */
#include "PreservedFiles.h"
#include "ff.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <new>

/* **
 * read - Read one file's size and, when _contents is true, calculate a checksum
 * of all its bytes. scratch provides 16 KB of temporary memory for each read.
 * Pause one scheduler tick after each chunk so other tasks can run.
 *
 * When only checking names and sizes, use f_stat without opening the file; the
 * recorder may already have it open. Return false on a file or read error. These
 * checks run outside timed acquisition.
 */
bool PreservedFiles::read(Entry& entry, uint8_t* scratch)
{
    // Metadata-only checks must not reopen a file the recorder already has open.
    if (!_contents)
    {
        FILINFO info = {};
        const bool retVal = f_stat(entry._path.c_str(), &info) == FR_OK && !(info.fattrib & AM_DIR);
        entry._length = info.fsize;
        entry._checksum = 0;
        return retVal;
    }
    // Calls are sequential on the disk task; close this handle before reuse.
    bool retVal = f_open(&_file, entry._path.c_str(), FA_READ) == FR_OK;
    if (retVal)
    {
        entry._length = f_size(&_file);
        entry._checksum = 0;
        uint64_t offset = 0;
        while (_contents && retVal && offset < entry._length)
        {
            const UINT amount = static_cast<UINT>(std::min<uint64_t>(16384, entry._length - offset));
            UINT actual = 0;
            retVal = f_read(&_file, scratch, amount, &actual) == FR_OK && actual == amount;
            if (retVal)
            {
                entry._checksum = esp_rom_crc32_le(entry._checksum, scratch, actual);
                offset += actual;
            }
            vTaskDelay(1);
        }
        const bool closed = f_close(&_file) == FR_OK;
        retVal = retVal && closed;
    }
    return retVal;
}

/* **
 * scan - List files under path and record their sizes and optional checksums.
 * Recurse into subdirectories, ignoring the navigation entries . and ...
 *
 * Return false on a read error, more than 4096 files, or a directory depth
 * greater than eight. These are software limits on this test's memory use, not
 * SD-card limits. Exceeding a limit must fail the check rather than silently
 * leave files unchecked.
 */
bool PreservedFiles::scan(const std::string& path, unsigned depth, uint8_t* scratch)
{
    FF_DIR directory = {};
    bool retVal = depth <= 8 && f_opendir(&directory, path.c_str()) == FR_OK;
    const bool opened = retVal;
    bool ended = false;
    while (retVal && !ended)
    {
        FILINFO info = {};
        retVal = f_readdir(&directory, &info) == FR_OK;
        ended = !retVal || info.fname[0] == '\0';
        if (!ended && strcmp(info.fname, ".") != 0 && strcmp(info.fname, "..") != 0)
        {
            const std::string child = path + "/" + info.fname;
            if (info.fattrib & AM_DIR)
            {
                retVal = scan(child, depth + 1, scratch);
            }
            else
            {
                Entry entry;
                entry._path = child;
                retVal = _entries.size() < 4096 && read(entry, scratch);
                if (retVal)
                {
                    _entries.push_back(entry);
                }
            }
        }
    }
    if (opened)
    {
        const bool closed = f_closedir(&directory) == FR_OK;
        retVal = retVal && closed;
    }
    return retVal;
}

/* **
 * capture - Save a list of the card's current files before the test. contents
 * selects full-file checksums or names and sizes only. Allocate temporary read
 * memory, scan from drive, then free it. Return false if allocation or scanning
 * fails, or if no files were found.
 */
bool PreservedFiles::capture(const char* drive, bool contents)
{
    _entries.clear();
    _drive = drive;
    _contents = contents;
    auto* scratch = static_cast<uint8_t*>(heap_caps_aligned_alloc(64, 16384, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    const bool retVal = scratch && scan(std::string(drive), 0, scratch) && !_entries.empty();
    heap_caps_free(scratch);
    return retVal;
}

/* **
 * verify - Check that files saved by capture still have the same sizes and, when
 * enabled, the same checksums. In names-and-sizes mode, also require the total
 * file count to be unchanged.
 *
 * Full-content mode allows newly added files, because addFiles is expected to
 * create them. Read or allocation failures fail verification.
 */
bool PreservedFiles::verify()
{
    auto* scratch = static_cast<uint8_t*>(heap_caps_aligned_alloc(64, 16384, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    bool retVal = scratch != nullptr;
    if (!_contents)
    {
        // A complete rescan detects new files as well as removals; individual
        // checks below establish that every original path still has its size.
        // The checker owns a FatFs sector cache. Allocate this temporary checker
        // outside the task stack too; allocation failure rejects verification.
        std::unique_ptr<PreservedFiles> preservedFilesCurrent(new (std::nothrow) PreservedFiles);
        retVal = retVal && preservedFilesCurrent && preservedFilesCurrent->capture(_drive.c_str(), false) &&
                 preservedFilesCurrent->getCount() == getCount();
    }
    for (const auto& original : _entries)
    {
        if (retVal)
        {
            Entry current;
            current._path = original._path;
            retVal = read(current, scratch) && current._length == original._length && current._checksum == original._checksum;
        }
    }
    heap_caps_free(scratch);
    return retVal;
}

/* **
 * getCount - Number of old files included in the completed baseline, not newly
 * allocated files.
 */
uint32_t PreservedFiles::getCount() const
{
    return static_cast<uint32_t>(_entries.size());
}
