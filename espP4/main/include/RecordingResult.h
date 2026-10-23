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
 * @file RecordingResult.h
 * Receiver and decoder results, independent of console or Feather messages.
 */
#pragma once
#include "CaptureStats.h"
#include "ReceiverStats.h"

/**
 * Stable snapshot after receiving stops and queued decoding finishes.
 * Clean refers to acquisition checks, not completed SD verification. SD writing
 * and readback can still fail later. The final partial hardware block can be lost.
 */
struct RecordingResult
{
    enum class Reason
    {
        requested,
        fault,
        timeout
    };
    Reason _reason = Reason::requested;
    uint32_t _run = 0, _sampleRate = 0, _slots = 0;
    uint32_t _pendingBits = 0, _commandErrors = 0;
    double _observedRate = 0;
    bool _clean = false;
    CaptureStats _captureStats;
    ReceiverStats _receiverStats;
};
