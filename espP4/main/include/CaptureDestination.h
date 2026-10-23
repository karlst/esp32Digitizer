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
 * @file CaptureDestination.h
 * Submission interface shared by recording and optional memory destinations.
 */
#pragma once
#include <cstdint>
#include "Ads127Config.h"
#include <cstddef>

/**
 * Accept complete decoder batches without retaining their pointers.
 * Core 0 requests prepare with run parameters, then polls getReady/getFailed.
 * True from prepare means accepted, not ready. cancelPreparation must prevent
 * a late completion from permitting capture; it must not block on disk I/O.
 * finish follows draining Core 1 and applies only to a ready destination.
 * Only Core 1 calls submit. getFailed/getFull are polled on Core 0 during capture;
 * implementations must synchronize any state shared between cores.
 * A destination must outlive the permanent decoder task. True from submit means
 * accepted, not written to disk. A full destination requests source shutdown.
 */
class CaptureDestination
{
public:
    /* **
     * ~CaptureDestination - Permit destruction through the interface.
     */
    virtual ~CaptureDestination() = default;
    virtual bool prepare(uint32_t run, const Ads127Config& adsConfigRequested) = 0;
    virtual bool getReady() const = 0;
    virtual void cancelPreparation() = 0;
    virtual bool submit(const uint32_t* words, size_t count) = 0;
    virtual bool getFailed() const = 0;
    virtual void finish(uint32_t run, uint32_t rate, bool clean, uint32_t samples) = 0;

    /* **
     * getFull - Unlimited storage does not request early source shutdown.
     */
    virtual bool getFull() const
    {
        return false;
    }
};
