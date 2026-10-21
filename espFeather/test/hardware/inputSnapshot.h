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

/** @file InputSnapshot.h
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
class InputSnapshot
{
public:
    bool initialize();
    void reset();
    void begin();
    void poll();
    bool getSampling() const;
    bool getFailed() const;
    void report(unsigned long run, bool hardwareClean);
private:

    /**
     * @brief Measurements taken together during one scheduled A2 read interval.
     * timeUs is the midpoint, in microseconds since begin() after the START echo;
     * durationUs is the time spent reading. sum/reads gives the average raw ADC
     * count; minimum and maximum preserve variation that the average would hide.
     */
    struct Point
    {
        uint32_t _timeUs;
        uint32_t _durationUs;
        uint32_t _reads;
        uint32_t _sum;
        int _minimum;
        int _maximum;
    };
    Point _points[320] = {};
    unsigned _count = 0;
    uint32_t _missed = 0, _maxGapUs = 0;
    int64_t _origin = 0, _nextRead = 0, _lastRead = 0;
    bool _collecting = false, _broken = false;
};
#endif
