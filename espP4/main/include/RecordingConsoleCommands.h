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

/** @file RecordingConsoleCommands.h
 * @brief Optional extra console commands supplied by test startup, not by an observer.
 */
#pragma once
class RecordingApplication;

/** @brief Core 0 command extension; production need not provide one. */
class RecordingConsoleCommands
{
public:

    /** @brief Permit destruction through the interface. */
    virtual ~RecordingConsoleCommands() = default;
    virtual void command(RecordingApplication& recordingApplication, char key) = 0;
};
