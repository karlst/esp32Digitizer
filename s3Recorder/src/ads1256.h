/**
 * @file ads1256.h
 * @brief Declare the ADS1256 hardware driver used only by the acquisition task.
 * Implementation and command-by-command explanations are in ads1256.cpp.
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
    /**
     * @brief Reset/configure/calibrate the chip and leave it in standby.
     */
    bool begin();
    /**
     * @brief Wake/configure the stopped chip; rate is samples per second.
     */
    bool start(uint32_t rate);
    /**
     * @brief Cease reads; false means hardware standby could not be confirmed.
     */
    bool stop();
    /**
     * @brief Deselect the ADC without SPI if the controller itself stopped responding.
     * Requires acquisition interrupts detached; recovery requires an S3 reboot.
     */
    void abandon();
    /**
     * @brief Read one ready 24-bit sample; discard sample if false is returned.
     */
    bool read(int32_t& sample);
    /**
     * @brief Return saved result code in low byte and read time in upper bits.
     */
    uint32_t readDiagnostic() const;
    /**
     * @brief Return microseconds spent inside the last three-byte SPI library call.
     * This includes library overhead and any preemption, not just clocks on the wire.
     */
    uint32_t transferMicros() const;
private:
    bool waitReady(uint32_t timeoutMs);
    bool configure(uint32_t rate);
    void command(uint8_t opcode);
    void readRegisters(uint8_t* values);
    // FSPI selects an S3 hardware SPI controller; it is not a software bit loop.
    SPIClass bus{FSPI};
    // continuous means driver active in either mode. continuousRead selects the
    // chip mode that omits a separate read-data command before each sample.
    bool continuous = false;
    bool continuousRead = false;
    uint32_t readDetail = 0;
    uint32_t transferUs = 0;
};
