/**
 * @file ads1256.cpp
 * @brief ADS1256 SPI sequencing based on TI SBAS288K, timing and command tables.
 * Reference: https://www.ti.com/lit/ds/symlink/ads1256.pdf
 */
#include "ads1256.h"
#include "commandProtocol.h"

/**
 * @brief Wait with a deadline; absent hardware must never hang commands indefinitely.
 */
bool ads1256::waitReady(uint32_t timeoutMs)
{
    const uint32_t started = millis();
    while (digitalRead(readyPin) != LOW && millis() - started < timeoutMs)
    {
        delay(1);
    }
    const bool retVal = digitalRead(readyPin) == LOW;
    return retVal;
}

/**
 * @brief Send one command with a conservative inter-command delay.
 */
void ads1256::command(uint8_t opcode)
{
    bus.transfer(opcode);
    // Seven microseconds exceeds both t6=50 clocks and SYNC t11=24 clocks
    // at 7.68 MHz. Keep CS asserted until the command/data sequence is done.
    delayMicroseconds(7);
}

/**
 * @brief Read STATUS/MUX/ADCON/DRATE in one transaction, respecting RREG t6.
 */
void ads1256::readRegisters(uint8_t* values)
{
    bus.transfer(0x10);
    bus.transfer(3);
    delayMicroseconds(7);
    uint8_t zeros[4] = {};
    bus.transferBytes(zeros, values, 4);
    delayMicroseconds(1);
}

/**
 * @brief Write and verify gain-one differential settings, then self-calibrate.
 */
bool ads1256::configure(uint32_t rate)
{
    uint8_t rateByte = 0;
    bool retVal = commandProtocol::rateRegister(rate, rateByte);
    if (retVal)
    {
        // Buffer off accepts a grounded negative input without buffer headroom
        // assumptions. ACAL off lets us explicitly wait for one self-calibration.
        // Clock output and sensor-test current sources are unused and disabled.
        const uint8_t settings[] = {0x50, 3, 0x00, 0x01, 0x00, rateByte};
        bus.writeBytes(settings, sizeof(settings));
        delayMicroseconds(7);
        uint8_t actual[4] = {};
        readRegisters(actual);
        // TI does not specify a fixed ID nibble: test writable bits/readback,
        // not the often-assumed ID=3. MUX/DRATE reject all-zero/all-one wiring.
        retVal = (actual[0] & 0x0e) == 0 && actual[1] == 0x01 &&
            actual[2] == 0 && actual[3] == rateByte;
        if (retVal)
        {
            command(0xf0);
            delayMicroseconds(10);
            retVal = waitReady(1000);
        }
    }
    return retVal;
}

/**
 * @brief Recover the serial interface, verify configuration, and leave ADC stopped.
 */
bool ads1256::begin()
{
    pinMode(readyPin, INPUT_PULLUP);
    pinMode(10, OUTPUT);
    digitalWrite(10, HIGH);
    bus.begin(12, 13, 11, 10);
    // This bus is exclusively owned by this worker for its lifetime. Keeping
    // its transaction open avoids a mutex/configuration round trip per sample.
    bus.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE1));
    digitalWrite(10, LOW);
    command(0x00);
    bool retVal = waitReady(1000);
    if (retVal)
    {
        // SDATAC also exits a previous firmware's continuous mode after an S3-only reset.
        command(0x0f);
        command(0xfe);
        delay(5);
        retVal = waitReady(1000) && configure(1000);
    }
    command(0xfd);
    digitalWrite(10, HIGH);
    return retVal;
}

/**
 * @brief Apply a rate while stopped; use explicit reads at low rates and RDATAC at high rates.
 */
bool ads1256::start(uint32_t rate)
{
    // Low-rate bench operation favors generous clock margin on jumper wires.
    // Higher rates need the shorter transfers; their throughput remains unverified.
    bus.endTransaction();
    bus.beginTransaction(SPISettings(rate <= 2000 ? 1000000 : 1900000, MSBFIRST, SPI_MODE1));
    digitalWrite(10, LOW);
    command(0x00);
    bool retVal = waitReady(1000) && configure(rate);
    if (retVal)
    {
        // At <=2 kS/s there is ample time to frame every result with RDATA.
        // This gives the low-rate bring-up path an explicit read boundary rather
        // than depending on RDATAC remaining synchronized between conversions.
        // High rates retain RDATAC to avoid spending their 33 us budget on commands.
        continuousRead = rate > 2000;
        if (continuousRead)
        {
            command(0x03);
        }
        continuous = true;
    }
    else
    {
        command(0xfd);
        digitalWrite(10, HIGH);
    }
    return retVal;
}

/**
 * @brief Exit RDATAC when used, then put the converter in standby at DRDY low.
 */
bool ads1256::stop()
{
    bool retVal = true;
    if (continuous)
    {
        retVal = waitReady(100);
        if (retVal)
        {
            if (continuousRead)
            {
                command(0x0f);
            }
            command(0xfd);
        }
        // Even on failure we cease SCLK/read activity. Without a working DRDY
        // wire we cannot prove ADC standby, so the caller records an ADC fault.
        continuous = false;
    }
    digitalWrite(10, HIGH);
    return retVal;
}

/**
 * @brief Read one complete conversion; the worker checks for overlapping DRDY edges.
 */
bool ads1256::read(int32_t& sample)
{
    // Keep fault evidence without printing in the sample path: low byte is the
    // stage (1=not active, 2=not ready, 3=DRDY stayed low, 0=complete), upper
    // bits are elapsed microseconds for an attempted SPI transfer.
    readDetail = continuous ? 2 : 1;
    bool retVal = continuous && digitalRead(readyPin) == LOW;
    if (retVal)
    {
        // Transmit zeros: 0x0f/0xfe on DIN would leave continuous mode.
        // Four-byte storage accommodates the ESP32 SPI implementation's word access.
        uint8_t zeros[4] = {};
        uint8_t bytes[4] = {};
        const uint32_t startedUs = micros();
        if (!continuousRead)
        {
            // command() includes the RDATA t6 delay before any result clocks.
            command(0x01);
        }
        bus.transferBytes(zeros, bytes, 3);
        sample = commandProtocol::signedSample(bytes);
        retVal = digitalRead(readyPin) == HIGH;
        readDetail = ((micros() - startedUs) << 8) | (retVal ? 0U : 3U);
    }
    return retVal;
}

/** @brief Return the last read stage/timing without accessing ADC hardware again. */
uint32_t ads1256::readDiagnostic() const
{
    const uint32_t retVal = readDetail;
    return retVal;
}
