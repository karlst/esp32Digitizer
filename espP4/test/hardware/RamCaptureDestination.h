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

/** @file RamCaptureDestination.h
 * @brief Test-only adapter that summarizes decoded samples without accessing SD.
 */
#pragma once
#include "CaptureDestination.h"
#include "RamWaveform.h"
/** @brief Own a short waveform summary; Core 0 reads only after decoder drain. */
class RamCaptureDestination : public CaptureDestination
{
public:
    /** @brief RAM mode rejects SD preparation requests. */
    bool prepare() override { return false; }
    /** @brief Reset the summary before Core 1 receives any new raw blocks. */
    bool claim(uint32_t run, uint32_t stage, uint32_t rate) override
    { (void)run; (void)stage; _ramWaveform.reset(rate); return true; }
    /** @brief Consume the batch immediately; no pointer survives this call. */
    bool submit(const uint32_t* words, size_t count) override
    { _ramWaveform.accept(words, count); return true; }
    /** @brief This bounded summary has no asynchronous storage operation to fail. */
    bool failed() const override { return false; }
    /** @brief Read the summary's atomic completion flag from Core 0. */
    bool full() const override { return _ramWaveform.complete(); }
    /** @brief Export only after all Core 1 decoding has stopped. */
    void finish(uint32_t run, uint32_t rate, bool clean, uint32_t samples) override
    { (void)samples; _ramWaveform.report(run, rate, clean); }
private:
    RamWaveform _ramWaveform;
};
