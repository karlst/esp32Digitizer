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
 * @file LedBlinker.h
 * @brief Nonblocking GPIO 13 LED control.
 */
#pragma once
#include <Arduino.h>

/**
 * @brief Store the legacy LED test's requested state and current output level.
 * No task or timer is created here. webApp calls update() from Arduino loop(),
 * and HTTP callbacks in that same task call setEnabled(). GPIO13 is a Feather
 * pin; it is unrelated to receiver GPIO13 used for digitizer data.
 */
class LedBlinker
{
public:
    /**
     * @brief Configure the LED in its stopped, off state.
     */
    void begin();
    /**
     * @brief Apply any LED transition due at the current time.
     */
    void update();
    /**
     * @brief Start or stop blinking; repeated commands preserve timing.
     */
    void setEnabled(bool enabled);
    /**
     * @brief Report commanded blinking, regardless of current LED level.
     */
    bool isEnabled() const;

private:
    // Toggle every half second to produce one complete blink per second.
    static constexpr uint8_t _ledPin = 13;
    static constexpr unsigned long _blinkIntervalMs = 500;
    // Requested activity and physical level differ during each off half-cycle.
    bool _blinkEnabled = false;
    bool _ledOn = false;
    unsigned long _lastToggleMs = 0;
};
