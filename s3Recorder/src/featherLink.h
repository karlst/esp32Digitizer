/**
 * @file featherLink.h
 * @brief Dedicated command/status UART, separate from USB debug output.
 */
#pragma once
#include "acquisition.h"

/**
 * @brief Exchange protocol-v2 frames without moving sample data through the Feather.
 */
class featherLink
{
public:
    explicit featherLink(acquisition& recorder);
    void begin();
    void update();
private:
    friend class nativeChecks;
    bool report(const acquisitionStatus& status);
    acquisition& recorder;
    commandProtocol parser;
    commandProtocol usbParser;
    uint32_t lastReportMs = 0;
    uint32_t sentAckId = 0;
    uint32_t sentAckResult = 0;
    uint32_t rebootSentMs = 0;
    uint32_t lastDebugMs = 0;
    bool rebootSent = false;
    uint32_t receivedBytes = 0;
    uint32_t validCommands = 0;
    uint32_t queuedCommands = 0;
};
