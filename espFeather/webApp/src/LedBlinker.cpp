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
 * @file LedBlinker.cpp
 * @brief Retained Feather LED test, controlled by legacy /api/blink routes.
 * The current browser page no longer has a blink button. This service still
 * initializes with blinking off and is updated from webApp in Arduino loop().
 * It uses elapsed time, not a sleeping task or interrupt, so it cannot hold up
 * browser handling by waiting half a second for the next transition.
 */
#include "LedBlinker.h"

/**
 * @brief Prepare GPIO 13 as an output and establish the stopped/off state.
 *
 * Write LOW before selecting output mode to start with the intended pin level.
 * Record the current time so a future Start has a known timing origin.
 */
void LedBlinker::begin()
{
    // Boot with the output low and blinking disabled.
    digitalWrite(_ledPin, LOW);
    pinMode(_ledPin, OUTPUT);
    _blinkEnabled = false;
    _ledOn = false;
    _lastToggleMs = millis();
}

/**
 * @brief Change the LED level if blinking is enabled and half a second has elapsed.
 *
 * Called frequently from webApp::update(), not by a timer interrupt. Return at once
 * if nothing is due. If the web loop was delayed, toggle once at the current time
 * instead of replaying many overdue flashes. Unsigned elapsed-time subtraction
 * continues to work when the milliseconds-since-boot counter wraps.
 */
void LedBlinker::update()
{
    // Unsigned subtraction handles millis rollover without special cases.
    const unsigned long nowMs = millis();
    if (_blinkEnabled && nowMs - _lastToggleMs >= _blinkIntervalMs)
    {
        _lastToggleMs = nowMs;
        _ledOn = !_ledOn;
        digitalWrite(_ledPin, _ledOn ? HIGH : LOW);
    }
}

/**
 * @brief Start with the LED immediately on, or stop with it immediately off.
 * @param enabled Whether repeated blinking is requested.
 *
 * Repeated requests for the existing state leave timing unchanged. All callers
 * and update() share the Arduino loop task, so this state needs no mutex.
 */
void LedBlinker::setEnabled(bool enabled)
{
    // Start immediately with the LED on; stopping leaves it off.
    if (_blinkEnabled != enabled)
    {
        _blinkEnabled = enabled;
        _ledOn = enabled;
        _lastToggleMs = millis();
        digitalWrite(_ledPin, _ledOn ? HIGH : LOW);
    }
}

/**
 * @brief Return whether blinking was requested, not the current light level.
 * @return True throughout a blinking run, including its half-second off periods.
 */
bool LedBlinker::isEnabled() const
{
    // Expose the commanded state to the controller.
    const bool retVal = _blinkEnabled;
    return retVal;
}
