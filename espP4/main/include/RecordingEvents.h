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
 * @file RecordingEvents.h
 * Control notifications independent of any particular command transport.
 */
#pragma once
#include "RecordingResult.h"
#include "Ads127Config.h"

/**
 * Let a controller stop its source and report outcomes on Core 0.
 * These synchronous callbacks never run on Core 1 or in an interrupt. They
 * must not re-enter recording operations. A destination-full notification asks
 * the controller to stop the source; it does not confirm that the source stopped.
 */
class RecordingEvents
{
public:
    enum class StartFailure
    {
        preparation,
        preparationTimeout,
        receiver,
        cancelled,
        commandLink
    };

    /* **
     * ~RecordingEvents - Permit destruction through the interface.
     */
    virtual ~RecordingEvents() = default;
    virtual void ready(uint32_t run, const Ads127Config& adsConfigRequested) = 0;
    virtual void startFailed(uint32_t run, StartFailure reason, int error) = 0;
    virtual void destinationFull(uint32_t run) = 0;
    virtual void stopped(const RecordingResult& recordingResult) = 0;
    virtual void decoderStalled(uint32_t run) = 0;
};
