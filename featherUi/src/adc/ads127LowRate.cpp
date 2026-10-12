/**
 * @file ads127LowRate.cpp
 * @brief Set a selected 16-1024 ksample/s bench rate, channel 0, status+sample output.
 * Read configure() for the complete register plan. Each successful write is
 * echoed by the ADC then independently read back. TI ADS127L1x Rev B register
 * sections 8.7-8.18 define these fields. Clock math assumes the EVM's documented
 * 32.768 MHz oscillator; measured S3 word rate must later check that assumption.
 */
#include "ads127LowRate.h"
#include <Arduino.h>
#include "ads127BenchRate.h"

/** @brief Remember the SPI driver without touching hardware. */
ads127LowRate::ads127LowRate(ads127l18& adc) : adc(adc)
{
}

/**
 * @brief Write and verify a persistent setting, printing the exact failure.
 * readback starts at 0xff so a failed transfer cannot masquerade as a zero match.
 */
bool ads127LowRate::setAndCheck(uint8_t address, uint8_t value)
{
    uint8_t readback = 0xff;
    const bool retVal = adc.writeRegister(address, value) &&
        adc.readRegister(address, readback) && readback == value;
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
bool ads127LowRate::readStatus(const char* phase, uint8_t& status)
{
    const bool retVal = adc.readRegister(0x02, status);
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
bool ads127LowRate::reportStatus(const char* phase)
{
    uint8_t status = 0xff;
    const bool retVal = readStatus(phase, status) && (status & 0x7f) == 0;
    if (!retVal)
    {
        configured = false;
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
bool ads127LowRate::clearStartupFlags()
{
    uint8_t status = 0xff;
    bool retVal = readStatus("before startup clear", status) && (status & 0x1f) == 0;
    if (retVal && (status & 0x60))
    {
        retVal = adc.writeRegister(0x02, 0x60);
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
 * @brief Identify and stop the ADC, then configure the requested bench stage.
 * stage indexes shared ads127BenchRates; default zero retains the 16-kHz test.
 * Returning true means settings read back correctly, not that DMA receives data.
 * On failure make a best-effort stop and refuse start(). No reset is issued, so
 * startup flags are logged before clearing; other fault flags are never cleared.
 * A power cycle restores defaults.
 */
bool ads127LowRate::configure(unsigned stage)
{
    configured = false;
    if (stage >= ads127BenchRateCount)
    {

        // Reject an unsupported request before any SPI or indexing operation.
        return false;
    }
    const ads127BenchRate& rate = ads127BenchRates[stage];
    adc.begin();
    uint8_t device = 0;
    bool retVal = adc.readRegister(0, device) && device == 0x06 && stop() && clearStartupFlags();

    // All register writes occur stopped. Max-speed mode accommodates 32.768 MHz;
    // modulator clock = 32.768 MHz / 2; selected OSR determines output rate.
    // OSR means the number of modulator readings combined into one output sample.
    // GEN_CFG3 bit 7 selects output drive: zero gives full strength, matching
    // TI's specified output timing. This speeds signal transitions without
    // changing their 1.8-V logic level. The full-drive high-clock tests still failed;
    // register readback alone cannot establish that received bits are correct.
    if (retVal)
    {
        retVal = setAndCheck(0x09, 0x06) &&  // Max speed; software start/stop; no averaging.
                 setAndCheck(0x08, 0x00) &&  // Board's default 2.5-V reference range.
                 setAndCheck(0x0a, 0x00) &&  // 24 bits; full drive; SPI CRC remains off.
                 setAndCheck(0x0d, 0x08) &&  // CLK_SEL bit 3 selects external EVM clock; divider 1.
                 setAndCheck(0x0c, rate.clockDividerCode) && // DCLK /8, /4, /2 or /1; default bit delay.
                 setAndCheck(0x0b, 0x70) &&  // Eight lanes; status header; no data CRC.
                 setAndCheck(0x10, 0x00) &&  // No GPIO override of sample outputs.
                 setAndCheck(0x11, 0x20);    // Internal short: coax signal not needed.
    }

    // Eight-lane mode puts channel zero alone on DOUT0, even though we connect
    // only that lane. This avoids turning unused output pins into floating inputs.
    // Every channel uses the same filter code; channels 1-7 additionally power down.
    for (uint8_t channel = 0; channel < 8 && retVal; ++channel)
    {
        retVal = setAndCheck(static_cast<uint8_t>(0x12 + channel * 8),
                            rate.filterCode | (channel == 0 ? 0 : 0x20));
    }
    if (retVal)
    {

        // Reassert stop after configuration: register changes can restart filter
        // sequencing. The later start must still wait for the S3 READY reply.
        retVal = stop() && reportStatus("configured, before ARM");
    }
    if (!retVal)
    {
        Serial.println("ADC configuration FAILED; attempting STOP; no START will be sent.");
        stop();
    }
    configured = retVal;
    return retVal;
}

/**
 * @brief Request conversions only after successful configure() and S3 readiness.
 * CONTROL bit 1 self-clears. Its write echo is not proof of clock/sample delivery.
 */
bool ads127LowRate::start()
{
    const bool retVal = configured && adc.writeRegister(0x07, 0x02);
    return retVal;
}

/**
 * @brief Request stop using CONTROL bit 0; safe to request even when already idle.
 * A true echo confirms acceptance; an in-progress conversion can finish afterward.
 */
bool ads127LowRate::stop()
{
    const bool retVal = adc.writeRegister(0x07, 0x01);
    return retVal;
}
