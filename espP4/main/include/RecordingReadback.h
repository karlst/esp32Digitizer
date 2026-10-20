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

/** @file RecordingReadback.h
 * @brief Optional observer of bytes actually read back from the SD card.
 */
#pragma once
#include <cstdint>
#include <cstddef>
/**
 * @brief Observe verification without owning the file or changing stored samples.
 * SdRecorder calls these methods only on its disk task, after capture ends.
 * The observer must outlive the recorder. accept must consume bytes before
 * returning because the recorder reuses its scratch memory on the next read.
 */
class RecordingReadback
{
public:
    /** @brief Allow destruction through the interface. */
    virtual ~RecordingReadback() = default;
    virtual void reset(uint32_t rate) = 0;
    virtual void accept(const uint8_t* data, size_t count) = 0;
    virtual void report(uint32_t run, bool clean) const = 0;
};
