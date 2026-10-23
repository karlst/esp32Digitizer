// Copyright (C) 2026 Karl Stock
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file MemoryRecordingDisk.cpp
 * RAM filesystem with short-write and flush faults for the real backend.
 * Only operations the backend uses are modeled. These tests cannot measure card
 * timing, FAT allocation behavior, power-loss durability or ESP task scheduling.
 */
#include "MemoryRecordingDisk.h"
#include "ff.h"
#include <algorithm>
#include <cstring>
#include <set>
static std::map<std::string, std::vector<uint8_t>> fileContents;
static std::set<std::string> directories;
static int writeFault = 0;
static bool syncFault = false;
static std::vector<MemoryRecordingDisk::Write> writes;

/* **
 * getWrites - Read the requested write sizes/positions; timestamps are
 * deliberately not modeled.
 */
const std::vector<MemoryRecordingDisk::Write>& MemoryRecordingDisk::getWrites()
{
    return writes;
}

/* **
 * clearWrites - Start a fresh observation window after file preparation.
 */
void MemoryRecordingDisk::clearWrites()
{
    writes.clear();
}

/* **
 * prepare - Mount succeeds; only explicit formatting clears existing RAM files.
 */
bool MemoryRecordingDisk::prepare(bool format)
{
    if (format)
    {
        fileContents.clear();
        directories.clear();
        writes.clear();
        writeFault = 0;
        syncFault = false;
    }
    directories.insert("0:");
    return true;
}

/* **
 * getDrive - Fixed logical drive for these tests.
 */
const char* MemoryRecordingDisk::getDrive() const
{
    return "0:";
}

/* **
 * files - Permit tests to seed/corrupt storage and compare complete before/after
 * copies.
 */
std::map<std::string, std::vector<uint8_t>>& MemoryRecordingDisk::files()
{
    return fileContents;
}

/* **
 * failWrite - Fail the next write completely or return a successful but short
 * prefix.
 */
void MemoryRecordingDisk::failWrite(bool shortWrite)
{
    writeFault = shortWrite ? 2 : 1;
}

/* **
 * failSync - Fail exactly the next f_sync call.
 */
void MemoryRecordingDisk::failSync()
{
    syncFault = true;
}

/* **
 * f_open - CREATE_NEW refuses existing paths, unlike truncating creation.
 */
FRESULT f_open(FIL* file, const char* path, BYTE mode)
{
    FRESULT retVal = FR_OK;
    const bool exists = fileContents.count(path) != 0;
    if ((mode & FA_CREATE_NEW) && exists)
    {
        retVal = FR_EXIST;
    }
    else if (!exists && !(mode & FA_CREATE_NEW))
    {
        retVal = FR_NO_FILE;
    }
    else
    {
        if (!exists)
        {
            fileContents[path] = {};
        }
        file->_path = path;
        file->_position = 0;
        file->_mode = mode;
        file->_open = true;
    }
    return retVal;
}

/* **
 * f_close - Close is explicit; subsequent operations reject this cursor.
 */
FRESULT f_close(FIL* file)
{
    file->_open = false;
    return FR_OK;
}

/* **
 * f_read - Read up to EOF and report the actual count, as FatFs does.
 */
FRESULT f_read(FIL* file, void* data, UINT length, UINT* actual)
{
    *actual = 0;
    const FRESULT retVal = file->_open && (file->_mode & FA_READ) ? FR_OK : FR_DISK_ERR;
    if (retVal == FR_OK)
    {
        const auto& bytes = fileContents.at(file->_path);
        *actual = static_cast<UINT>(std::min<uint64_t>(length, bytes.size() - file->_position));
        if (*actual)
        {
            memcpy(data, bytes.data() + file->_position, *actual);
        }
        file->_position += *actual;
    }
    return retVal;
}

/* **
 * f_write - Copy into the reserved file and inject a one-shot I/O/short-write
 * failure.
 */
FRESULT f_write(FIL* file, const void* data, UINT length, UINT* actual)
{
    writes.push_back({file->_position, length});
    *actual = 0;
    FRESULT retVal = file->_open && (file->_mode & FA_WRITE) && writeFault != 1 ? FR_OK : FR_DISK_ERR;
    if (retVal == FR_OK)
    {
        *actual = writeFault == 2 ? length / 2 : length;
        auto& bytes = fileContents.at(file->_path);
        if (file->_position + *actual > bytes.size())
        {
            bytes.resize(static_cast<size_t>(file->_position + *actual));
        }
        if (*actual)
        {
            memcpy(bytes.data() + file->_position, data, *actual);
        }
        file->_position += *actual;
    }
    writeFault = 0;
    return retVal;
}

/* **
 * f_sync - Simulate a synchronization result without any physical device.
 */
FRESULT f_sync(FIL* file)
{
    const FRESULT retVal = file->_open && !syncFault ? FR_OK : FR_DISK_ERR;
    syncFault = false;
    return retVal;
}

/* **
 * f_lseek - Seek only within allocated space, catching accidental extension by
 * the backend.
 */
FRESULT f_lseek(FIL* file, FSIZE_t position)
{
    const FRESULT retVal = file->_open && position <= fileContents.at(file->_path).size() ? FR_OK : FR_DISK_ERR;
    if (retVal == FR_OK)
    {
        file->_position = position;
    }
    return retVal;
}

/* **
 * f_expand - Reserve zero-filled payload in RAM; firmware FatFs reserves real
 * clusters.
 */
FRESULT f_expand(FIL* file, FSIZE_t length, BYTE)
{
    fileContents.at(file->_path).resize(static_cast<size_t>(length));
    return FR_OK;
}

/* **
 * f_size - Return allocated file length, not its valid recording payload length.
 */
FSIZE_t f_size(FIL* file)
{
    return fileContents.at(file->_path).size();
}

/* **
 * f_mkdir - Remember a directory without adding it to the file-content map.
 */
FRESULT f_mkdir(const char* path)
{
    return directories.insert(path).second ? FR_OK : FR_EXIST;
}

/* **
 * f_opendir - Enumerate immediate file children; current backend uses one flat
 * directory.
 */
FRESULT f_opendir(FF_DIR* directory, const char* path)
{
    const FRESULT retVal = directories.count(path) ? FR_OK : FR_NO_FILE;
    directory->_paths.clear();
    directory->_next = 0;
    const std::string prefix = std::string(path) + "/";
    for (const auto& entry : fileContents)
    {
        if (entry.first.rfind(prefix, 0) == 0 && entry.first.find('/', prefix.size()) == std::string::npos)
        {
            directory->_paths.push_back(entry.first);
        }
    }
    return retVal;
}

/* **
 * f_readdir - Return one 8.3 filename or the empty end marker expected by
 * scan().
 */
FRESULT f_readdir(FF_DIR* directory, FILINFO* info)
{
    *info = {};
    if (directory->_next < directory->_paths.size())
    {
        const std::string path = directory->_paths[directory->_next++];
        const std::string name = path.substr(path.find_last_of('/') + 1);
        memcpy(info->fname, name.c_str(), name.size() + 1);
        info->fsize = fileContents.at(path).size();
    }
    return FR_OK;
}

/* **
 * f_closedir - Release the test directory snapshot.
 */
FRESULT f_closedir(FF_DIR* directory)
{
    directory->_paths.clear();
    return FR_OK;
}
