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
 * @file SyntheticRecordingLayout.h
 * Four-byte known-pattern format used only by the SD write test.
 */
#pragma once
#include "RecordingLayout.h"
#include "sdTestData.h"
#include <cstdio>

/**
 * Keep generated-pattern expectations out of the real SD recorder.
 */
class SyntheticRecordingLayout : public RecordingLayout
{
public:
    /* **
     * getRecordBytes - Generated test words occupy four bytes each.
     */
    uint32_t getRecordBytes() const override
    {
        return 4;
    }

    /* **
     * countMatches - The generator checks its own total before finish; retain that
     * contract.
     */
    bool countMatches(uint64_t bytes, uint32_t samples) const override
    {
        (void)samples;
        return bytes % 4 == 0;
    }

    /* **
     * check - Compare every complete card-read word with its expected sequence
     * position.
     */
    uint64_t check(const uint8_t* data, uint32_t bytes, uint64_t offset) const override
    {
        return sdTestData::checkBlock(data, bytes, offset / 4);
    }

    /* **
     * report - Print the number of mismatching generated words found on the card.
     */
    void report(uint64_t errors) const override
    {
        printf("SD TEST patternErrors=%llu\n", static_cast<unsigned long long>(errors));
    }
};
