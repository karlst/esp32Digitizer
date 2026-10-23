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
 * @file RamCaptureDestination.h
 * Test-only adapter that summarizes decoded samples without accessing SD.
 */
#pragma once
#include "CaptureDestination.h"
#include "RamWaveform.h"

/**
 * Own a short waveform summary; Core 0 reads only after decoder drain.
 */
class RamCaptureDestination : public CaptureDestination
{
public:
    /* **
     * prepare - Reset the RAM summary immediately; no disk work or delayed readiness
     * exists.
     */
    bool prepare(uint32_t run, const Ads127Config& adsConfigRequested) override
    {
        (void)run;
        _ramWaveform.reset(adsConfigRequested._samplesPerSecond);
        _ready = true;
        return true;
    }

    /* **
     * getReady - Core 0 reads readiness set by prepare; this flag is not shared with
     * Core 1.
     */
    bool getReady() const override
    {
        return _ready;
    }

    /* **
     * cancelPreparation - Cancel before receiving begins; no background work needs
     * draining.
     */
    void cancelPreparation() override
    {
        _ready = false;
    }

    /* **
     * submit - Consume the batch immediately; no pointer survives this call.
     */
    bool submit(const uint32_t* words, size_t count) override
    {
        _ramWaveform.accept(words, count);
        return true;
    }

    /* **
     * getFailed - This bounded summary has no asynchronous storage operation to
     * fail.
     */
    bool getFailed() const override
    {
        return false;
    }

    /* **
     * getFull - Read the summary's atomic completion flag from Core 0.
     */
    bool getFull() const override
    {
        return _ramWaveform.getComplete();
    }

    /* **
     * finish - Export only after all Core 1 decoding has stopped.
     */
    void finish(uint32_t run, uint32_t rate, bool clean, uint32_t samples) override
    {
        (void)samples;
        _ramWaveform.report(run, rate, clean);
    }

private:
    bool _ready = false;
    RamWaveform _ramWaveform;
};
