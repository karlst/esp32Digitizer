/**
 * @file SPI.h
 * @brief Define a pretend SPI controller for desktop tests.
 * driverChecks.cpp implements these methods and interprets driver commands in
 * memory. nativeChecks.cpp replaces ADC methods instead, so it only needs this
 * type to construct the acquisition object. Neither executable accesses SPI pins.
 */
#pragma once
#include <cstdint>
#include <cstddef>
#define FSPI 0
#define HSPI 1
#define MSBFIRST 1
#define SPI_MODE1 1
/**
 * @brief Retain the driver's chosen clock and mode for assertions.
 */
struct SPISettings
{
    uint32_t frequency;
    int order;
    int mode;
    /**
     * @brief Store requested bus settings.
     */
    SPISettings(uint32_t frequency, int order, int mode) : frequency(frequency), order(order), mode(mode) {}
};
/**
 * @brief Provide the driver member's type without opening hardware.
 */
class SPIClass
{
public:
    /**
     * @brief Retain construction compatibility only.
     */
    explicit SPIClass(int) {}
    void begin(int clock, int input, int output, int chipSelect);
    void beginTransaction(SPISettings settings);
    void endTransaction();
    uint8_t transfer(uint8_t byte);
    void transferBytes(const uint8_t* input, uint8_t* output, uint32_t count);
    void writeBytes(const uint8_t* bytes, uint32_t count);
};
