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

/** @file AdcReceiver.h
 * @brief Host replacement for receive hardware, with explicit injected outcomes.
 */
#pragma once
#include "ReceiverStats.h"
#include "esp_err.h"
#include "esp_timer.h"

/** @brief Record calls without DMA, queues or interrupts; hardware tests cover those. */
class AdcReceiver
{
public:
    esp_err_t _startResult = ESP_OK;
    bool _failed = false;
    unsigned _starts = 0;
    uint32_t _clockHz = 0;
    ReceiverStats _receiverStats;

    /** @brief No resources exist in this fake. */
    void initialize() {}

    /** @brief Record requested clock rate and return the injected setup result. */
    esp_err_t start(uint32_t clockHz) { ++_starts; _clockHz = clockHz; return _startResult; }

    /** @brief Record receiver shutdown for ordering checks. */
    void stop() { controlFake::operations += "stop "; }

    /** @brief Return an injected live receive failure. */
    bool getFailed() const { return _failed; }

    /** @brief Return the injected frozen measurements. */
    ReceiverStats getReceiverStats() const { return _receiverStats; }
};
