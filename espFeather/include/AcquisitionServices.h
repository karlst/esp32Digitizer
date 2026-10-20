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
 * @file AcquisitionServices.h
 * @brief Optional signal and measurement services supplied by the startup program.
 */
#pragma once
/**
 * @brief Coordinate an external source with ADC capture without owning that source.
 * AcquisitionApplication calls these methods on Arduino's single setup/loop task.
 * start precedes ADC START; finish follows ADC STOP and the receiver STOP request.
 * A successful method confirms software acceptance only, not an accurate voltage.
 * The default service uses an already-connected external source and collects no
 * local measurements. Test startup supplies a subclass for its DAC and A2 reader.
 * Services must outlive the application; no method is called from an interrupt.
 */
class AcquisitionServices
{
public:
    /** @brief Allow deletion through the interface; this does not stop hardware. */
    virtual ~AcquisitionServices() = default;
    /** @brief Prepare optional hardware without starting a signal. */
    virtual bool initialize() { return true; }
    /** @brief Select the ADC's external channel rather than its internal short. */
    virtual bool externalInput() const { return true; }
    /** @brief Print optional operator information; idle describes controller state. */
    virtual void describe(bool idle) { (void)idle; }
    /** @brief Clear optional measurement state before asking P4 to receive. */
    virtual void reset() {}
    /** @brief Start an optional source after P4 confirms readiness. */
    virtual bool start() { return true; }
    /** @brief Start optional measurement after ADC START is acknowledged. */
    virtual void begin() {}
    /** @brief Collect any pending observations without printing during capture. */
    virtual void poll() {}
    /** @brief Report a latched service failure to the controller. */
    virtual bool failed() { return false; }
    /** @brief Request frequent polling while collecting a short measurement burst. */
    virtual bool sampling() const { return false; }
    /** @brief Stop the source and report observations after ADC stop was requested. */
    virtual void finish(unsigned long runId, bool clean) { (void)runId; (void)clean; }
};
