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

/** @file inputSnapshot.h
 * @brief Independent A2 observations during P4 RAM or SD smoke experiments.
 * Arduino loop owns all fields; the DAC timer never accesses this object.
 * The A1->A2 jumper measures the physical voltage, not the requested DAC code.
 */
#pragma once
#if ADS127_COMPARE_INPUT
#include <Arduino.h>

/** @brief Fixed-size timestamped input record, printed only after ADC/DAC stop.
 * A2 is Feather GPIO34 / ADC1 channel 6. Counts are uncalibrated 12-bit values.
 * This sensor limits comparison accuracy; it is not a reference-grade ADC.
 */
class inputSnapshot
{
public:
    bool initialize();
    void reset();
    void begin();
    void poll();
    bool sampling() const;
    bool failed() const;
    void report(unsigned long run, bool hardwareClean);
private:
    /**
     * @brief Measurements taken together during one scheduled A2 read interval.
     * timeUs is the midpoint, in microseconds since begin() after the START echo;
     * durationUs is the time spent reading. sum/reads gives the average raw ADC
     * count; minimum and maximum preserve variation that the average would hide.
     */
    struct point
    {
        uint32_t timeUs;
        uint32_t durationUs;
        uint32_t reads;
        uint32_t sum;
        int minimum;
        int maximum;
    };
    point points[320] = {};
    unsigned count = 0;
    uint32_t missed = 0, maxGapUs = 0;
    int64_t origin = 0, nextRead = 0, lastRead = 0;
    bool collecting = false, broken = false;
};
#endif
