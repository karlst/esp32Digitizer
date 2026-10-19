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
 * @file benchDacWave.h
 * @brief Functional-test sine or opt-in constant code, independent of hardware APIs.
 * Nominal 10 Hz, 1.25 V offset, 0.5 V peak: 0.75..1.75 V before DAC error and
 * output loading. The Feather's 8-bit DAC and nominal 3.3 V supply are not a
 * calibrated signal source. This is not a precision ADC noise/distortion test.
 */
#pragma once
#include <cmath>
#include <cstdint>

namespace benchDacWave
{
// Microseconds: one sine cycle lasts 100 ms; writes are requested every 0.5 ms.
constexpr uint64_t periodUs = 100000;
constexpr uint64_t updateUs = 500;

/**
 * @brief Return the current sine code, or fixed 135 in the constant diagnostic.
 * A delayed update skips ahead rather than slowing the requested waveform.
 * Modulo before floating conversion preserves phase precision at long uptime.
 * elapsedUs is time since start, not a sample index. The return is an 8-bit DAC
 * command, not a measured voltage; constant mode ignores elapsedUs.
 */
inline uint8_t codeAt(uint64_t elapsedUs)
{
#if ADS127_DAC_CONSTANT
    // Hold the former sine peak (135 * 3.3 / 255 = about 1.747 V).
    // This avoids the observed low-voltage flattening. The timer still writes
    // every 500 us so scheduling remains comparable with the sine experiment.
    // A constant command is not proof of a constant physical voltage.
    (void)elapsedUs;
    const uint8_t retVal = 135;
#else
    const double phase = 6.2831853071795864769 * (elapsedUs % periodUs) / periodUs;
    const double volts = 1.25 + 0.5 * std::sin(phase);
    const uint8_t retVal = static_cast<uint8_t>(std::lround(volts * 255.0 / 3.3));
#endif
    return retVal;
}
}
