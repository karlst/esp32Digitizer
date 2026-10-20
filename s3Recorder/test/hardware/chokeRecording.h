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
 * @file chokeRecording.h
 * @brief Synthetic S3 recording identification and requested generator rates.
 * The real sink writes the file. This helper only adds test metadata to its
 * zero-filled 512-byte header; it never touches the card or changes acquisition.
 */
#pragma once
#include "chokeTest.h"
#include <cstddef>
#include <cstdint>

namespace chokeRecording
{
constexpr const char* magic = "S3CHK001";

/**
 * @brief Append requested stored-bit rates and stage milliseconds, not measured rates.
 * putLittle is the sink's shared integer encoder. bytes points to its header;
 * bytes 56 through 67 are reserved for this test's three four-byte fields.
 */
inline void appendHeader(uint8_t* bytes, void (*putLittle)(uint8_t*, uint64_t, size_t))
{
    putLittle(bytes + 56, chokeTest::initialBps, 4);
    putLittle(bytes + 60, chokeTest::fixedRate ? 0 : chokeTest::incrementBps, 4);
    putLittle(bytes + 64, chokeTest::stageUs / 1000, 4);
}
}
