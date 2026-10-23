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
 * @file RecordingLayout.h
 * File-layout policy borrowed by the SD recorder for one recording.
 */
#pragma once
#include "recordingFormat.h"

/**
 * Describe the number of bytes in each stored sample and any optional data
 * check. The default is three bytes per ADC sample. SdRecorder selects this
 * object during preparation and uses it until saving and readback finish, so the
 * caller must keep it alive.
 *
 * The disk task passes bytes read from the card to check. Tests can supply known
 * patterns through this interface without putting test logic in the recorder.
 */
class RecordingLayout
{
public:
    /* **
     * ~RecordingLayout - Allow destruction through this interface.
     */
    virtual ~RecordingLayout() = default;

    /* **
     * getRecordBytes - Return bytes per stored record.
     */
    virtual uint32_t getRecordBytes() const
    {
        return 3;
    }

    /* **
     * countMatches - Require exactly one stored record per reported source sample.
     */
    virtual bool countMatches(uint64_t bytes, uint32_t samples) const
    {
        return bytes == static_cast<uint64_t>(samples) * getRecordBytes();
    }

    /* **
     * check - Optional content check; normal ADC data have no predetermined values.
     */
    virtual uint64_t check(const uint8_t* data, uint32_t bytes, uint64_t offset) const
    {
        (void)data;
        (void)bytes;
        (void)offset;
        return 0;
    }

    /* **
     * report - Optional extra result text, printed after file closure.
     */
    virtual void report(uint64_t errors) const
    {
        (void)errors;
    }
};
