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

/** @file ControlDestination.h
 * @brief A destination that separates preparation acceptance from readiness.
 */
#pragma once
#include "CaptureDestination.h"
#include "esp_timer.h"

/** @brief Inject storage state while exercising the real RecordingApplication sequencing. */
class ControlDestination : public CaptureDestination
{
public:
    bool _ready = false, _failed = false, _full = false, _clean = false;
    bool _acceptPreparation = true, _cancelled = false;
    unsigned _preparations = 0, _finishes = 0;
    uint32_t _samples = 0;
    Ads127Config _adsConfig = adsConfig;

    /** @brief Accept setup without completing it; the test supplies later readiness. */
    bool prepare(uint32_t, const Ads127Config& adsConfigRequested) override
    {
        ++_preparations;
        if (_acceptPreparation) { _ready = false; _cancelled = false; _failed = false; _adsConfig = adsConfigRequested; }
        return _acceptPreparation;
    }

    /** @brief Readiness cannot revive a cancelled pending request. */
    bool getReady() const override { return _ready && !_cancelled; }

    /** @brief Record cancellation without pretending the disk operation completed. */
    void cancelPreparation() override { _cancelled = true; }

    /** @brief Lifecycle tests inject decoder results and submit no actual samples. */
    bool submit(const uint32_t*, size_t) override { return true; }

    /** @brief Return the injected write failure. */
    bool getFailed() const override { return _failed; }

    /** @brief Return the injected full-RAM condition. */
    bool getFull() const override { return _full; }

    /** @brief Record completion after receiver stop, drain and controller notification. */
    void finish(uint32_t, uint32_t, bool clean, uint32_t samples) override
    {
        controlFake::operations += "finish ";
        ++_finishes;
        _clean = clean;
        _samples = samples;
    }
};
