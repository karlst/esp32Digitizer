/**
 * @file acquisitionStatus.h
 * @brief A copied status record, never a live view into mutable acquisition data.
 */
#pragma once
#include <cstdint>

/**
 * @brief Confirmed state plus diagnostics; sample count is cumulative until S3 reboot.
 */
struct acquisitionStatus
{
    bool ready = false;
    bool running = false;
    bool hasSample = false;
    bool reboot = false;
    uint32_t rate = 1000;
    uint32_t measuredRate = 0;
    uint64_t sampleCount = 0;
    int32_t latestRaw = 0;
    uint32_t lastSampleMs = 0;
    uint32_t error = 0;
    uint32_t ackId = 0;
    uint32_t ackResult = 0;
    uint64_t missedEdges = 0;
    // USB-only fault details: bit0 incomplete SPI read, bit1 DRDY edge during read.
    uint32_t readFault = 0;
    uint32_t readDetail = 0;
};
