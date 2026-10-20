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
 * @file webController.h
 * @brief HTTP routes for page delivery and hardware commands.
 */
#pragma once
#include <WebServer.h>
#include "../test/hardware/ledBlinker.h"
#include "dacGenerator.h"
#include "s3Monitor.h"

/**
 * @brief Connect URL routes to the application services, without owning their hardware.
 * All handlers execute in Arduino's loop task when WebServer services a request.
 * The generator supplies its own synchronization for its separate worker. The
 * S3 monitor uses this same loop task, so handlers need no extra monitor lock.
 * See webController.cpp for endpoint setup and accepted-versus-confirmed replies.
 */
class webController
{
public:
    /**
     * @brief Bind to services owned by the application.
     */
    webController(WebServer& server, ledBlinker& blinker, dacGenerator& generator, s3Monitor& monitor);
    /**
     * @brief Register page, asset, and API routes before server startup.
     */
    void begin();
    /**
     * @brief Execute an acknowledged reboot after allowing the response to leave.
     */
    void update();

private:
    /**
     * @brief Stream the landing page or report a missing asset.
     */
    void serveIndex();
    /**
     * @brief Respond with authoritative blink state as uncached JSON.
     */
    void sendBlinkState();
    /**
     * @brief Send settings and timestamped DAC/ADC graph data.
     */
    void sendDacState();
    /**
     * @brief Validate submitted settings and start output atomically.
     */
    void startDac();
    /**
     * @brief Validate an S3 command and queue it for UART acknowledgement.
     */
    void commandS3(const char* action);
    /**
     * @brief Parse a required finite decimal form parameter without accepting trailing junk.
     */
    bool readNumber(const char* name, float& value);

    // The application owns these services and outlives route callbacks.
    WebServer& server;
    ledBlinker& blinker;
    dacGenerator& generator;
    s3Monitor& monitor;
    // The HTTP reply is sent first; update() checks this flag/time to restart later.
    bool rebootPending = false;
    unsigned long rebootRequestedMs = 0;
};
