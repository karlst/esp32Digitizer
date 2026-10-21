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

/** @file FrameDecoder.h
 * @brief Inject decoder results for lifecycle checks; real decoding has its own suites.
 */
#pragma once
#include "CaptureStats.h"

/** @brief A result holder, not a second implementation of the decoding algorithm. */
class FrameDecoder
{
public:
    CaptureStats _captureStats;
    bool _outputFailed = false;

    /** @brief Return values arranged by the test. */
    const CaptureStats& getCaptureStats() const { return _captureStats; }

    /** @brief This fake has no unfinished raw word. */
    uint32_t getPendingBits() const { return 0; }

    /** @brief Inject refusal by a destination. */
    bool getOutputFailed() const { return _outputFailed; }
};
