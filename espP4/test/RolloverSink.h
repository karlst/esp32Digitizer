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

/** @file RolloverSink.h
 * @brief RolloverSink declaration shared by its callers.
 */
#pragma once
#include "ByteSink.h"
#include <cassert>
#include <cstring>
/** @brief Mock destination verifies every block before the caller reuses its source. */
class RolloverSink : public ByteSink
{
public:
    const uint8_t* _expected = nullptr;
    size_t _expectedBytes = 0;
    uint64_t _total = 0;

    /** @brief Compare copied ring data with the requested block and count accepted bytes. */
    size_t write(const uint8_t* data, size_t length) override
    {
        assert(length == _expectedBytes && std::memcmp(data, _expected, length) == 0);
        _total += length;
        return length;
    }

    /** @brief No hardware is present; synchronization succeeds in this boundary test. */
    bool flush() override { return true; }
};
