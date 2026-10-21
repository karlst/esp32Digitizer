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
 * @file Ads127LowRate.cpp
 * @brief Set a selected 16-1024 ksample/s bench rate, channel 0, status+sample output.
 * Read configure() for the complete register plan. Each successful write is
 * echoed by the ADC then independently read back. TI ADS127L1x Rev B register
 * sections 8.7-8.18 define these fields. Clock math assumes the EVM's documented
 * 32.768 MHz oscillator; measured receiver word rate must later check that assumption.
 */
#include "Ads127LowRate.h"
#include <Arduino.h>
#include "Ads127Config.h"

/** @brief Remember the SPI driver without touching hardware. */
Ads127LowRate::Ads127LowRate(Ads127L18& ads127L18) : _ads127L18(ads127L18)
{
}

/**
 * @brief Write and verify a persistent setting, printing the exact failure.
 * readback starts at 0xff so a failed transfer cannot masquerade as a zero match.
 */
bool Ads127LowRate::setAndCheck(uint8_t address, uint8_t value)
{
    uint8_t readback = 0xff;
    const bool retVal = _ads127L18.writeRegister(address, value) &&
        _ads127L18.readRegister(address, readback) && readback == value;
    Serial.printf("ADC register 0x%02X: wanted 0x%02X, read 0x%02X, %s\n",
                  address, value, readback, retVal ? "OK" : "FAILED");
    return retVal;
}

/**
 * @brief Read SPI STATUS and explain each bit on Feather USB without clearing it.
 * phase labels when this snapshot was taken. Bits 6/5 retain low-voltage/reset
 * history, including normal power-up; bits 4..0 describe other ADC/SPI faults.
 * A completed SPI read is not independent proof that the physical link is sound.
 */
bool Ads127LowRate::readStatus(const char* phase, uint8_t& status)
{
    const bool retVal = _ads127L18.readRegister(0x02, status);
    if (retVal)
    {
        Serial.printf("ADC STATUS %s: 0x%02X lowVoltageHistory=%u resetHistory=%u spiCrc=%u registerCrc=%u internalAdc=%u address=%u spiClockCount=%u\n",
            phase, status, (status >> 6) & 1, (status >> 5) & 1,
            (status >> 4) & 1, (status >> 3) & 1, (status >> 2) & 1,
            (status >> 1) & 1, status & 1);
    }
    else
    {
        Serial.printf("ADC STATUS %s: read FAILED\n", phase);
    }
    return retVal;
}

/**
 * @brief Print a status snapshot; return true only when no defined flag is set.
 * Called after Stop by the bench controller to look for faults during the run.
 * This never clears evidence. A fault also prevents start() until reconfiguration.
 */
bool Ads127LowRate::reportStatus(const char* phase)
{
    uint8_t status = 0xff;
    const bool retVal = readStatus(phase, status) && (status & 0x7f) == 0;
    if (!retVal)
    {
        _configured = false;
    }
    return retVal;
}

/**
 * @brief Record then acknowledge only the two startup-history flags while stopped.
 * TI sections 7.4.9.1/8.3: writing ONE clears a STATUS flag; zero leaves it alone.
 * Thus 0x60 acknowledges bits 6/5, never the other fault bits. Ordinary register
 * equality readback is inappropriate: successful clearing reads zero, not 0x60.
 * Refuse setup on other faults, a rejected write, or any flag remaining afterward.
 * A later status/header check is still needed: this is only a pre-run snapshot.
 */
bool Ads127LowRate::clearStartupFlags()
{
    uint8_t status = 0xff;
    bool retVal = readStatus("before startup clear", status) && (status & 0x1f) == 0;
    if (retVal && (status & 0x60))
    {
        retVal = _ads127L18.writeRegister(0x02, 0x60);
        if (!retVal)
        {
            Serial.println("ADC startup flag clear: write echo FAILED.");
        }
    }

    // Read even after rejection so USB retains the remaining fault evidence.
    const bool statusRead = readStatus("after startup clear", status);
    retVal = retVal && statusRead && (status & 0x7f) == 0;
    return retVal;
}

/**
 * @brief Identify and stop the ADC, then apply the supplied ADC configuration.
 * The default is adsConfig (1,024,000 samples/s); unsupported settings are refused.
 * externalInput=false retains the internal-short baseline. True selects the
 * physical AIN0P/AIN0N inputs, driven through the EVM's analog front end. This
 * selects CH0_CFG1's external input and enables the ADC's midpoint-voltage
 * output for the EVM input amplifiers. Sample rate and serial format are unchanged.
 * Returning true means settings read back correctly, not that DMA receives data.
 * On failure make a best-effort stop and refuse start(). No reset is issued, so
 * startup flags are logged before clearing; other fault flags are never cleared.
 * A power cycle restores defaults.
 */
bool Ads127LowRate::configure(const Ads127Config& adsConfigRequested, bool externalInput)
{
    _configured = false;
    if (!isSupportedAds127Config(adsConfigRequested))
    {

        // Reject an unsupported request before any SPI operation.
        return false;
    }
    _ads127L18.begin();
    uint8_t device = 0;
    bool retVal = _ads127L18.readRegister(0, device) && device == 0x06 && stop() && clearStartupFlags();

    // All register writes occur stopped. Max-speed mode accommodates 32.768 MHz;
    // modulator clock = 32.768 MHz / 2; selected OSR determines output rate.
    // OSR divides the internal 16.384-MHz conversion rate to produce the output rate.
    // GEN_CFG3 bit 7 selects output drive: zero gives full strength, matching
    // TI's specified output timing. This speeds signal transitions without
    // changing their 1.8-V logic level.
    // Register readback alone cannot establish that received bits are correct.
    if (retVal)
    {
        retVal = setAndCheck(0x09, 0x06) &&  // Max speed; software start/stop; no averaging.
                 // GEN_CFG1 bit 2 enables VCM: an analog output at the midpoint
                 // of the ADC supply rails. The TI board connects this output to
                 // the input amplifiers to establish their output center voltage.
                 // Enable it for external-input tests; retain the internal-short
                 // baseline otherwise. Bit 0 remains zero for the 2.5-V reference.
                 // Readback confirms the setting, not the actual analog voltage
                 // or that this change fixes the distorted recorded waveform.
                 setAndCheck(0x08, externalInput ? 0x04 : 0x00) &&
                 setAndCheck(0x0a, 0x00) &&  // 24 bits; full drive; SPI CRC remains off.
                 setAndCheck(0x0d, 0x08) &&  // CLK_SEL bit 3 selects external EVM clock; divider 1.
                 setAndCheck(0x0c, adsConfigRequested._clockDividerCode) && // DCLK /8, /4, /2 or /1; default bit delay.
                 setAndCheck(0x0b, 0x70) &&  // Eight lanes; status header; no data CRC.
                 setAndCheck(0x10, 0x00) &&  // No GPIO override of sample outputs.
                 // TI Rev B table 8-20: MUX[6:4]=010 shorts internally; 000
                 // uses normal external polarity. Range and buffer bits stay 0.
                 setAndCheck(0x11, externalInput ? 0x00 : 0x20);
    }

    // Eight-lane mode puts channel zero alone on DOUT0, even though we connect
    // only that lane. This avoids turning unused output pins into floating inputs.
    // Every channel uses the same filter code; channels 1-7 additionally power down.
    for (uint8_t channel = 0; channel < 8 && retVal; ++channel)
    {
        retVal = setAndCheck(static_cast<uint8_t>(0x12 + channel * 8),
                            adsConfigRequested._filterCode | (channel == 0 ? 0 : 0x20));
    }
    if (retVal)
    {

        // Reassert stop after configuration: register changes can restart filter
        // sequencing. The later start must still wait for the receiver READY reply.
        retVal = stop() && reportStatus("configured, before ARM");
    }
    if (!retVal)
    {
        Serial.println("ADC configuration FAILED; attempting STOP; no START will be sent.");
        stop();
    }
    _configured = retVal;
    return retVal;
}

/**
 * @brief Request conversions after successful configure(); return whether the echo matches.
 * The caller must establish receiver readiness first. This function checks
 * configured but has no access to the receiver's READY message.
 * CONTROL bit 1 self-clears. Its write echo is not proof of clock/sample delivery.
 */
bool Ads127LowRate::start()
{
    const bool retVal = _configured && _ads127L18.writeRegister(0x07, 0x02);
    return retVal;
}

/**
 * @brief Request stop using CONTROL bit 0; safe to request even when already idle.
 * A true echo confirms acceptance; an in-progress conversion can finish afterward.
 */
bool Ads127LowRate::stop()
{
    const bool retVal = _ads127L18.writeRegister(0x07, 0x01);
    return retVal;
}
