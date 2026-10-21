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

/** @file RecordingReporter.h
 * @brief Report recording events without choosing a display or transport.
 */
#pragma once
#include "RecordingResult.h"
#include "BufferedWriter.h"

/**
 * @brief Receive status and completed results from the controller and SD task.
 * Calls occur on Core 0, never in the receive interrupt or Core 1 decoder.
 * Different Core 0 tasks can call this interface; an implementation that owns
 * mutable shared state must synchronize it. Borrowed arguments last only until
 * the call returns: a queued C6 sender must copy them. Required Feather replies
 * remain the controller's responsibility even if reporting is disabled.
 */
class RecordingReporter
{
public:

    /** @brief Permit destruction through the reporting interface. */
    virtual ~RecordingReporter() = default;
    enum class StorageError { close, mount, needsExfat, prepare };
    virtual void storagePrepared(bool ready, const char* path) const = 0;
    virtual void storageError(StorageError error, int code = 0) const = 0;
    virtual void storageReserved(uint64_t bytes) const = 0;
    virtual void storageVerifying(uint64_t bytes) const = 0;
    virtual void storageProgress(uint64_t bytes, uint64_t total) const = 0;
    virtual void storageFinished(bool ok, const char* path, uint64_t bytes, uint32_t crc,
        const BufferedWriter::Statistics& statistics, double writeMbps) const = 0;
    virtual void startup() const = 0;
    virtual void message(const char* text) const = 0;
    virtual void capture(const RecordingResult& recordingResult) const = 0;
};
