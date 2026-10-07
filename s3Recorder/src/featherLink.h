/**
 * @file featherLink.h
 * @brief Dedicated command/status UART, separate from USB debug output.
 */
#pragma once
#include "acquisition.h"

/**
 * @brief Exchange version-2 commands and version-3 status on the S3 side of the link.
 * main.cpp owns this object and calls it from the Arduino loop task. It holds a
 * reference to acquisition, sends it queued commands, and reads copied status.
 * Reports include only the latest sample and counters, not the full sample stream.
 * USB accepts the same command syntax for bench tests via its own parser.
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

    // Independent buffers: partial text from one port cannot join the other port.
    commandProtocol parser;
    commandProtocol usbParser;

    // Timing and acknowledgement tracking determine whether to report now or wait.
    // sentAck fields change only after a complete report is accepted by the UART.
    uint32_t lastReportMs = 0;
    uint32_t sentAckId = 0;
    uint32_t sentAckResult = 0;
    uint32_t rebootSentMs = 0;
    uint32_t lastDebugMs = 0;
#if S3_CHOKE_TEST
    uint32_t lastChokeDebugMs = 0; // Retry after TX congestion instead of losing every test line.
#endif

    // Once the reboot reply has been flushed, stop accepting further commands.
    bool rebootSent = false;

    // USB troubleshooting counters for the Feather input path only: physical
    // bytes seen, complete valid commands, and commands accepted into the queue.
    uint32_t receivedBytes = 0;
    uint32_t validCommands = 0;
    uint32_t queuedCommands = 0;
};
