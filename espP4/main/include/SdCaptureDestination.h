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

/** @file SdCaptureDestination.h
 * @brief Adapt the SD recorder to the decoder's destination interface.
 */
#pragma once
#include "CaptureDestination.h"
#include "SdRecorder.h"

/** @brief Borrow one recorder; all disk access remains on its own background task. */
class SdCaptureDestination : public CaptureDestination
{
public:

    /** @brief Store the recorder reference without touching hardware. */
    explicit SdCaptureDestination(SdRecorder& sdRecorder) : _sdRecorder(sdRecorder) {}

    /** @brief Request a new recording file; poll getReady before submitting samples. */
    bool prepare(uint32_t run, const Ads127Config& adsConfigRequested) override
    { return _sdRecorder.prepare(run, adsConfigRequested._samplesPerSecond); }

    /** @brief Confirm the asynchronous file setup finished for this recording. */
    bool getReady() const override { return _sdRecorder.getReady(); }

    /** @brief Prevent late readiness and ask the SD task to close the unused file. */
    void cancelPreparation() override { _sdRecorder.cancelPreparation(); }

    /** @brief Pack and copy samples into the recorder's ring without disk I/O. */
    bool submit(const uint32_t* words, size_t count) override
    { return _sdRecorder.submitSamples(words, count); }

    /** @brief Read the recorder's latched failure status. */
    bool getFailed() const override { return _sdRecorder.getFailed(); }

    /** @brief Request draining and verification after decoder submissions have stopped. */
    void finish(uint32_t run, uint32_t rate, bool clean, uint32_t samples) override
    { (void)run; (void)rate; _sdRecorder.finish(clean, samples); }
private:
    SdRecorder& _sdRecorder;
};
