/**
 * @file driverChecks.cpp
 * @brief Exercise the real SPI driver against a small command/register simulator.
 * This checks sequencing and fault paths, not electrical timing on a real board.
 */
#include "ads1256.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

uint32_t testMs = 0;
uint32_t testUs = 0;
int testReady = LOW;
int testChipSelect = HIGH;
static uint8_t registers[4] = {};
static bool missing = false;
static bool corruptReadback = false;
static bool registerRead = false;
static bool readCount = false;
static bool holdReadyLow = false;
static uint32_t readCommandUs = 0;
static std::vector<uint8_t> commands;

/**
 * @brief Simulate periodic DRDY edges while the stop code waits for a fresh one.
 */
static void clockReady()
{
    ++testUs;
    testReady = testUs % 33 < 3 ? HIGH : LOW;
}

/**
 * @brief Check that the implementation uses the user's traced SPI pins.
 */
void SPIClass::begin(int clock, int input, int output, int chipSelect)
{ assert(clock == 12 && input == 13 && output == 11 && chipSelect == 10); }
/**
 * @brief Check a legal clock/mode for the assumed 7.68 MHz module.
 */
void SPIClass::beginTransaction(SPISettings settings)
{ assert(settings.frequency <= 1920000 && settings.mode == SPI_MODE1 && settings.order == MSBFIRST); }
/**
 * @brief No actual bus semaphore is needed in the single-thread driver simulator.
 */
void SPIClass::endTransaction() {}
/**
 * @brief Simulate one command byte sent by the real digitizer driver.
 *
 * Assert chip select is low. Track register-read commands so the following count
 * byte is interpreted as a count, not another command. Standby raises fake DRDY;
 * wakeup/reset/calibration lower it unless the test says the chip is missing.
 * The zero returned here is dummy received data, not a confirmed ADC sample.
 */
uint8_t SPIClass::transfer(uint8_t byte)
{
    assert(testChipSelect == LOW);
    if (readCount)
    {
        assert(byte == 3);
        readCount = false;
        readCommandUs = testUs;
    }
    else
    {
        commands.push_back(byte);
        if (byte == 0x10) { registerRead = true; readCount = true; }
        if (byte == 0x03 || byte == 0x0f) { assert(testReady == LOW); }
        if (byte == 0xfd) { testReady = HIGH; }
        else if (!missing && (byte == 0 || byte == 0xfe || byte == 0xf0)) { testReady = LOW; }
    }
    return 0;
}
/**
 * @brief Supply a register reply or a three-byte sample to the real driver.
 *
 * For registers, require four bytes and the required command-to-read delay; optionally
 * corrupt one setting to prove readback checking works. For samples, return -8388608
 * and arrange for DRDY to rise two simulated microseconds later, or never rise when
 * holdReadyLow is set. This reproduces the condition behind the early-read fault.
 */
void SPIClass::transferBytes(const uint8_t* input, uint8_t* output, uint32_t count)
{
    assert(testChipSelect == LOW);
    for (uint32_t index = 0; index < count; ++index) { assert(input[index] == 0); }
    if (registerRead)
    {
        assert(count == 4 && testUs - readCommandUs >= 7);
        std::memcpy(output, registers, 4);
        if (corruptReadback) { output[3] ^= 1; }
        registerRead = false;
    }
    else
    {
        assert(count == 3);
        output[0] = 0x80; output[1] = 0; output[2] = 0;
        // Reproduce real hardware: DRDY need not be high at controller completion.
        testReady = LOW;
        testReadyRiseAt = holdReadyLow ? UINT32_MAX : testUs + 2;
    }
}
/**
 * @brief Capture the four configuration bytes that the real driver sends.
 *
 * Require WREG starting at zero and the four-register count. Add read-only ID bits
 * so validation must mask STATUS correctly rather than comparing all bits to zero.
 * Later simulated register reads return these saved values, not hardcoded success.
 */
void SPIClass::writeBytes(const uint8_t* bytes, uint32_t count)
{
    assert(count == 6 && bytes[0] == 0x50 && bytes[1] == 3);
    std::memcpy(registers, bytes + 2, 4);
    registers[0] |= 0x30;
}
/**
 * @brief Run driver setup, supported-rate reads, and deliberate hardware-fault simulations.
 *
 * First simulate an absent digitizer, then incorrect configuration readback, then
 * normal startup/read/stop at every supported rate. Finally hold DRDY low during
 * read and high during stop to verify bounded failures. These tests exercise actual
 * driver code but cannot establish real wire timing, signal integrity, or throughput.
 */
int main()
{
    // Setup must fail cleanly when DRDY never answers or settings read back wrong.
    ads1256 driver;
    missing = true;
    testReady = HIGH;
    assert(!driver.begin() && testMs >= 1000 && testChipSelect == HIGH);
    missing = false;
    corruptReadback = true;
    assert(!driver.begin());
    corruptReadback = false;
    assert(driver.begin() && testChipSelect == HIGH);
    int32_t value = 0;
    assert(!driver.read(value));
    // Each advertised rate must select a working command sequence. Low rates
    // request every sample; high rates enter continuous-read mode once.
    const uint32_t rates[] = {100,500,1000,2000,7500,15000,30000};
    for (uint32_t rate : rates)
    {
        assert(driver.start(rate));
        assert(registers[1] == 1 && registers[2] == 0);
        const size_t beforeRead = commands.size();
        assert(driver.read(value) && value == -8388608);
        if (rate <= 2000)
        {
            assert(commands.size() == beforeRead + 1 && commands.back() == 0x01);
            assert((driver.readDiagnostic() >> 8) >= 7);
        }
        else
        {
            assert(commands.size() == beforeRead && commands.back() == 0x03);
        }
        testReady = LOW;
        testClockHook = rate > 2000 ? clockReady : nullptr;
        assert(driver.stop() && commands.back() == 0xfd && testChipSelect == HIGH);
        testClockHook = nullptr;
    }
    // Unsupported settings and stuck ready levels must fail rather than hang.
    assert(!driver.start(250));
    assert(driver.start(1000));
    holdReadyLow = true;
    assert(!driver.read(value) && (driver.readDiagnostic() & 255) == 3);
    holdReadyLow = false;
    testReady = HIGH;
    const uint32_t before = testMs;
    assert(!driver.stop() && testMs - before >= 100 && testChipSelect == HIGH);
    std::cout << "PASS: ADC pins/mode, missing-device timeout, register readback, seven rates, signed read, standby and DRDY fault.\n";
    return 0;
}
