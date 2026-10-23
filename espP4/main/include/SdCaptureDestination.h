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
 * @file SdCaptureDestination.h
 * Adapt the SD recorder to the decoder's destination interface.
 */
#pragma once
#include "CaptureDestination.h"
#include "SdRecorder.h"

/**
 * Connect the capture interface to an existing SdRecorder. The caller keeps the
 * recorder alive. Sample submission copies into its ring buffer; its disk task
 * performs all card access.
 */
class SdCaptureDestination : public CaptureDestination
{
public:
    /* **
     * SdCaptureDestination - Store the recorder reference without using the
     * hardware.
     */
    explicit SdCaptureDestination(SdRecorder& sdRecorder)
        : _sdRecorder(sdRecorder)
    {
    }

    /* **
     * prepare - Ask the recorder to prepare its file pool for this run and sample
     * rate. True means the request was accepted; wait for getReady before submitting
     * samples.
     */
    bool prepare(uint32_t run, const Ads127Config& adsConfigRequested) override
    {
        return _sdRecorder.prepare(run, adsConfigRequested._samplesPerSecond);
    }

    /* **
     * getReady - Return true when the requested file-pool preparation has finished
     * and was not cancelled.
     */
    bool getReady() const override
    {
        return _sdRecorder.getReady();
    }

    /* **
     * cancelPreparation - Cancel a pending preparation. The disk task closes any
     * opened file; late completion must not start capture.
     */
    void cancelPreparation() override
    {
        _sdRecorder.cancelPreparation();
    }

    /* **
     * submit - Pack and copy samples into the recorder's ring without disk I/O.
     */
    bool submit(const uint32_t* words, size_t count) override
    {
        return _sdRecorder.submitSamples(words, count);
    }

    /* **
     * getFailed - Read the recorder's recorded failure status.
     */
    bool getFailed() const override
    {
        return _sdRecorder.getFailed();
    }

    /* **
     * getFull - Return whether the selected stopWhenFull file pool is full. The
     * controller uses this to request ADC shutdown. Samples rejected while shutdown
     * is pending still make the capture fail.
     */
    bool getFull() const override
    {
        return _sdRecorder.getFull();
    }

    /* **
     * finish - Request draining and verification after decoder submissions have
     * stopped.
     */
    void finish(uint32_t run, uint32_t rate, bool clean, uint32_t samples) override
    {
        (void)run;
        (void)rate;
        _sdRecorder.finish(clean, samples);
    }

private:
    SdRecorder& _sdRecorder;
};
