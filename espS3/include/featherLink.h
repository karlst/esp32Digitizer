/*
 * Copyright (C) 2026 Karl Stock
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of ESP32 Digitizer.
 * You may redistribute and/or modify it under the GNU General Public License
 * as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 *
 * Distributed WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License in the repository LICENSE file or <https://www.gnu.org/licenses/>.
 */

/**
 * @file featherLink.h
 * @brief Dedicated command/status UART, separate from USB debug output.
 */
#pragma once
#include "acquisition.h"

/**
 * @brief Receive version-2/3 commands and send version-5 S3 status to Feather.
 * S3_LEGACY_STATUS builds send version-2 status instead, without recording fields.
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
