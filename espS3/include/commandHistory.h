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
 * @file commandHistory.h
 * @brief Remember recent command results without repeating hardware actions.
 */
#pragma once
#include "commandProtocol.h"

/**
 * @brief Bounded replay protection for the single-command Feather protocol.
 * Keeps the last sixteen completed commands until S3 reboot. Reusing an id
 * with different contents is rejected, including after intervening commands.
 */
class commandHistory
{
public:
    bool lookup(const acquisitionCommand& command, uint32_t& result) const;
    void remember(const acquisitionCommand& command, uint32_t result);
private:
    // Parallel arrays: entries holds request contents, results its recorded answer.
    // Result zero marks an unused slot; next wraps to replace the oldest pair.
    // Only the acquisition task uses this object, so it needs no separate lock.
    acquisitionCommand entries[16];
    uint32_t results[16] = {};
    size_t next = 0;
};
