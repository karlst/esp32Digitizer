/**
 * @file ads127l18.h
 * @brief ADS127L18 configuration-port access for initial commissioning.
 * This is not the sample receiver. The separate receiver uses DCLK/FSYNC/DOUT0;
 * replacing that receiver does not change this configuration-port driver. The hardware diagnostic is the first caller of this class.
 */
#pragma once
#include <SPI.h>

/**
 * @brief Read/write ADC registers through slow SPI; construction never starts it.
 * The caller owns the SPI controller and calls begin() only after checking power.
 * Use from one task (Arduino loop in the diagnostic), not from an interrupt or
 * concurrent tasks. No lock is needed because that caller is the only bus user.
 * The ADC must be in SPI mode, with the power-on default SPI CRC disabled.
 * Writes are explicit caller actions; the separate read-only test never uses them.
 */
class ads127l18
{
public:
    ads127l18(SPIClass& bus, int clockPin, int inputPin, int outputPin, int selectPin);
    void begin();
    bool readRegister(uint8_t address, uint8_t& value);
    bool writeRegister(uint8_t address, uint8_t value);

private:
    SPIClass& bus;
    int clockPin;
    int inputPin;
    int outputPin;
    int selectPin;
    bool initialized = false;
    uint8_t transferFrame(uint8_t command, uint8_t data);
};
