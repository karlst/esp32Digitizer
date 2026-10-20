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

/** @file CaptureDestination.h
 * @brief Submission interface shared by recording and optional memory destinations.
 */
#pragma once
#include <cstdint>
#include <cstddef>
/**
 * @brief Accept complete decoder batches without retaining their pointers.
 * Core 0 calls prepare/claim before capture and finish after draining Core 1.
 * Only Core 1 calls submit. failed/full may be polled on Core 0 during capture;
 * implementations must synchronize any state shared between cores.
 * A destination must outlive the permanent decoder task. True from submit means
 * accepted, not written to disk. A full destination requests source shutdown.
 */
class CaptureDestination
{
public:
    /** @brief Permit destruction through the interface. */
    virtual ~CaptureDestination() = default;
    virtual bool prepare() = 0;
    virtual bool claim(uint32_t run, uint32_t stage, uint32_t rate) = 0;
    virtual bool submit(const uint32_t* words, size_t count) = 0;
    virtual bool failed() const = 0;
    virtual void finish(uint32_t run, uint32_t rate, bool clean, uint32_t samples) = 0;
    /** @brief Unlimited storage does not request early source shutdown. */
    virtual bool full() const { return false; }
};
