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

/** @file SyntheticRecordingLayout.h
 * @brief Four-byte known-pattern format used only by the SD write test.
 */
#pragma once
#include "RecordingLayout.h"
#include "sdTestData.h"
#include <cstdio>
/** @brief Keep generated-pattern expectations out of the real SD recorder. */
class SyntheticRecordingLayout : public RecordingLayout
{
public:
    /** @brief Generated test words occupy four bytes each. */
    uint32_t recordBytes() const override { return 4; }
    /** @brief The generator checks its own total before finish; retain that contract. */
    bool countMatches(uint64_t bytes, uint32_t samples) const override
    { (void)samples; return bytes % 4 == 0; }
    /** @brief Compare every complete card-read word with its expected sequence position. */
    uint64_t check(const uint8_t* data, uint32_t bytes, uint64_t offset) const override
    { return sdTestData::checkBlock(data, bytes, offset / 4); }
    /** @brief Write the original synthetic-test header, preserving existing reader compatibility. */
    void makeHeader(uint8_t* data, bool valid, uint32_t run, uint32_t stage,
                    uint32_t rate, uint64_t bytes, uint32_t crc) const override
    { sdTestData::makeHeader(data, valid, run, stage, rate, bytes, crc); }
    /** @brief Print the number of mismatching generated words found on the card. */
    void report(uint64_t errors) const override
    { printf("SD TEST patternErrors=%llu\n", static_cast<unsigned long long>(errors)); }
};
