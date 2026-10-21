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
 * @file Ads127Config.h
 * @brief ADC settings and receiver timing for this board's 32.768-MHz clock.
 * Each entry describes one complete supported configuration. Callers pass the
 * configuration itself, never its array position. adsConfig is the default.
 * The serial protocol carries samples/second; each board resolves that value
 * to the same supported configuration. Unknown rates are refused.
 */
#pragma once
#include <cstdint>

/**
 * @brief Settings needed to configure the ADC and interpret its output wire.
 * Feather writes the two encoded register values while the ADC is stopped.
 * P4 uses samples/second and slots to set up reception. These are hardware
 * settings, not measurements or proof that the requested rate was achieved.
 */
struct Ads127Config
{
    // Number of completed output samples per second, used by both boards.
    uint32_t _samplesPerSecond;

    // Oversampling ratio: internal conversion rate divided by output rate.
    // Here the internal rate is 16.384 MHz. For example, 16.384 MHz / 16
    // gives 1.024 million output samples/s. The digital filter performs this
    // reduction; it is not simply an average of independent readings.
    // This field documents/checks the setting encoded in _filterCode.
    uint16_t _osr;

    // Register bit pattern selecting the ADC's digital filter and its OSR.
    // This is an encoded choice, not the OSR number itself: 0x09 selects
    // OSR 16 in this configuration. Feather writes it to channel CFG1.
    uint8_t _filterCode;

    // Register bit pattern for DCLK, the clock that shifts output bits to P4.
    // Bits 6:5 select /1, /2, /4 or /8 of 32.768 MHz (0x00/20/40/60).
    // This changes bit-transfer speed, not the requested sampling rate.
    uint8_t _clockDividerCode;

    // Number of 32-clock intervals between sample starts on DOUT0. Only the
    // first contains our 8-bit status plus 24-bit sample. P4 checks this spacing.
    uint8_t _slots;

    /** @brief Compare all settings, so equal rates cannot hide different hardware choices. */
    constexpr bool matches(const Ads127Config& other) const
    {
        return _samplesPerSecond == other._samplesPerSecond && _osr == other._osr &&
            _filterCode == other._filterCode && _clockDividerCode == other._clockDividerCode &&
            _slots == other._slots;
    }
};

// Name the preferred configuration directly; changing table order cannot change it.
static constexpr Ads127Config ads127DefaultConfig = {1024000, 16, 0x09, 0x00, 1};
static constexpr const Ads127Config& adsConfig = ads127DefaultConfig;

// Alternatives remain available for explicit operator requests and driver checks.
static constexpr Ads127Config ads127Configs[] = {
    {16000, 1024, 0x10, 0x60, 8},
    {32000, 512, 0x0f, 0x60, 4},
    {64000, 256, 0x0e, 0x60, 2},
    {128000, 128, 0x0d, 0x40, 2},
    {256000, 64, 0x0c, 0x20, 2},
    {512000, 32, 0x0b, 0x00, 2},
    ads127DefaultConfig
};

/**
 * @brief Resolve an actual rate in samples/second; return null for unsupported rates.
 * The returned pointer refers to a permanent, read-only configuration. There
 * is exactly one supported configuration per rate in this table.
 */
inline const Ads127Config* getAds127Config(uint32_t samplesPerSecond)
{
    const Ads127Config* retVal = nullptr;
    for (const Ads127Config& candidate : ads127Configs)
    {
        if (candidate._samplesPerSecond == samplesPerSecond) { retVal = &candidate; }
    }
    return retVal;
}

/** @brief Refuse unknown or inconsistent settings before any hardware or storage action. */
inline bool isSupportedAds127Config(const Ads127Config& requestedConfig)
{
    const Ads127Config* supportedConfig = getAds127Config(requestedConfig._samplesPerSecond);
    return supportedConfig != nullptr && supportedConfig->matches(requestedConfig);
}
