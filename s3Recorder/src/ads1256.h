/**
 * @file ads1256.h
 * @brief One-owner SPI driver for the existing AIN0-minus-AIN1 test wiring.
 */
#pragma once
#include <Arduino.h>
#include <SPI.h>

/**
 * @brief Configure and read the ADC; only the acquisition worker may call this class.
 * GPIO9=DRDY, 10=CS, 11=DIN, 12=SCLK, 13=DOUT. PDWN is tied to 3.3 V,
 * not GPIO46. Timing assumes the module's standard 7.68 MHz ADC oscillator.
 */
class ads1256
{
public:
    static constexpr int readyPin = 9;
    bool begin();
    bool start(uint32_t rate);
    bool stop();
    bool read(int32_t& sample);
    uint32_t readDiagnostic() const;
private:
    bool waitReady(uint32_t timeoutMs);
    bool configure(uint32_t rate);
    void command(uint8_t opcode);
    void readRegisters(uint8_t* values);
    SPIClass bus{FSPI};
    bool continuous = false;
    bool continuousRead = false;
    uint32_t readDetail = 0;
};
