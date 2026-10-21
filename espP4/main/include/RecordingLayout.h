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

/** @file RecordingLayout.h
 * @brief File-layout policy borrowed by the SD recorder for one recording.
 */
#pragma once
#include "recordingFormat.h"

/**
 * @brief Describe stored record size, readback checks and the final file header.
 * The default is the normal three-byte ADC format. prepare() selects a borrowed
 * policy before producer submissions; it must remain alive through SD SAVED.
 * Disk-task callbacks see bytes read from the card, not the original RAM batch.
 * This interface allows other data sources without coupling the writer to tests.
 */
class RecordingLayout
{
public:

    /** @brief Allow destruction through this interface. */
    virtual ~RecordingLayout() = default;

    /** @brief Return bytes per stored record. */
    virtual uint32_t getRecordBytes() const { return 3; }

    /** @brief Require exactly one stored record per reported source sample. */
    virtual bool countMatches(uint64_t bytes, uint32_t samples) const
    { return bytes == static_cast<uint64_t>(samples) * getRecordBytes(); }

    /** @brief Optional content check; normal ADC data have no predetermined values. */
    virtual uint64_t check(const uint8_t* data, uint32_t bytes, uint64_t offset) const
    { (void)data; (void)bytes; (void)offset; return 0; }

    /** @brief Fill the existing 512-byte ADC file header after payload verification. */
    virtual void makeHeader(uint8_t* data, bool valid, uint32_t run,
                            uint32_t rate, uint64_t bytes, uint32_t crc) const
    { recordingFormat::makeHeader(data, valid, run, rate, bytes, crc); }

    /** @brief Optional extra result text, printed after file closure. */
    virtual void report(uint64_t errors) const { (void)errors; }
};
