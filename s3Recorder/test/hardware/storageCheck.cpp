/*
 * Copyright (C) 2026 Karl Stock
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of ESP32 Digitizer.
 * You may redistribute and/or modify it under the GNU General Public License
 * as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 *
 * Distributed WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License in the repository LICENSE file or <https://www.gnu.org/licenses/>.
 */

/**
 * @file storageCheck.cpp
 * @brief Optional S3 startup file write/read test, linked only by storage-check builds.
 * This member definition uses the real sink's filesystem; no recorder is copied.
 * recordingService calls it before acquisition begins when S3_SD_VERIFY is set.
 */
#include "sdRecordingSink.h"
#include <cstdio>

/**
 * @brief Write, close, reopen and check every byte of a separate 64-KiB test file.
 * Only the optional storage-check build calls this, on the disk task at startup.
 * A new filename is reserved exclusively; existing files are never overwritten.
 * The file remains on the card for inspection. Success verifies this one transfer,
 * not sustained speed or immunity to future power loss. No acquisition is running.
 */
bool sdRecordingSink::verifyStorage()
{
    bool retVal = mount();
    FsFile testFile;
    char path[40] = {};
    uint32_t id = 1;

    // Choose an unused name without assuming an empty card or deleting old tests.
    do
    {
        snprintf(path, sizeof(path), "/sdio_check_%06lu.bin", static_cast<unsigned long>(id++));
    } while (retVal && filesystem.exists(path) && id < 1000000);
    retVal = retVal && testFile.open(&filesystem, path, O_CREAT | O_EXCL | O_RDWR);
    uint8_t block[512];

    // Each byte depends on its absolute position, so misplaced sectors and bytes
    // are detected. write() is the actual file transfer; sync() commits caches.
    for (uint32_t offset = 0; retVal && offset < 65536; offset += sizeof(block))
    {
        for (size_t index = 0; index < sizeof(block); ++index)
        {
            const uint32_t position = offset + index;
            block[index] = static_cast<uint8_t>(position * 37 + (position >> 8) * 11);
        }
        retVal = testFile.write(block, sizeof(block)) == sizeof(block);
    }
    if (retVal)
    {
        retVal = testFile.sync();
    }
    testFile.close();

    // Reopen through the filesystem and check size and all payload bytes, rather
    // than relying only on the recording header/tail check used for large files.
    retVal = retVal && testFile.open(&filesystem, path, O_RDONLY);
    retVal = retVal && testFile.fileSize() == 65536;
    for (uint32_t offset = 0; retVal && offset < 65536; offset += sizeof(block))
    {
        retVal = testFile.read(block, sizeof(block)) == sizeof(block);
        for (size_t index = 0; retVal && index < sizeof(block); ++index)
        {
            const uint32_t position = offset + index;
            retVal = block[index] == static_cast<uint8_t>(position * 37 + (position >> 8) * 11);
        }
    }
    testFile.close();
    Serial.printf("SD full readback: %s bytes=65536 result=%s\n", path, retVal ? "PASS" : "FAIL");
    return retVal;
}
