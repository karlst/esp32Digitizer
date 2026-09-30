/**
 * @file spi_struct.h
 * @brief Desktop model of only the S3 SPI registers used by fastCapture.
 */
#pragma once
#include <cstdint>
/** @brief Register storage advanced by a test clock hook, not physical hardware. */
struct fakeSpiRegisters
{
    struct { uint32_t usr = 0; uint32_t update = 0; } cmd;
    struct { uint32_t ms_data_bitlen = 0; } ms_dlen;
    uint32_t data_buf[16] = {};
};
inline fakeSpiRegisters GPSPI2;
