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
 * @file ConsoleRecordingReporter.h
 * Format optional recording diagnostics for the P4 USB terminal.
 */
#pragma once
#include "RecordingReporter.h"

/**
 * Preserve the current terminal reports behind the reporting interface.
 * Formatting uses values supplied by the caller, never live sample buffers.
 * Disabling output does not disable required Feather control replies.
 */
class ConsoleRecordingReporter : public RecordingReporter
{
public:
    void storageRetention(uint64_t firstByte, uint64_t bytes, uint64_t total) const override;
    void storageSession(uint64_t session, uint32_t files, uint32_t rollovers,
        uint32_t reuses, uint64_t lostBytes) const override;

    /* **
     * ConsoleRecordingReporter - Choose whether to print diagnostics; construction
     * performs no I/O.
     */
    explicit ConsoleRecordingReporter(bool enabled = true)
        : _enabled(enabled)
    {
    }

    void storagePrepared(bool ready, const char* path) const override;
    void storageError(StorageError error, int code = 0) const override;
    void storageReserved(uint64_t bytes) const override;
    void storageVerifying(uint64_t bytes) const override;
    void storageProgress(uint64_t bytes, uint64_t total) const override;
    void storageFinished(bool ok, const char* path, uint64_t bytes, uint32_t crc,
        const BufferedWriter::Statistics& statistics, double writeMbps) const override;
    void startup() const override;
    void message(const char* text) const override;
    void capture(const RecordingResult& recordingResult) const override;

private:
    const bool _enabled;
};
