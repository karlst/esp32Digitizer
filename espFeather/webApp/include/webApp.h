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
 * @file webApp.h
 * @brief Application ownership and startup for the Feather UI.
 */
#pragma once
#include <WebServer.h>
#include "../../test/hardware/ledBlinker.h"
#include "webController.h"

/**
 * @brief Keep the Feather's services alive and call them in the required order.
 * This is the top-level application object owned by main.cpp. begin() performs
 * startup once; update() is called repeatedly by Arduino loop(). Only the DAC
 * generator creates a separate worker task. The controller holds references to
 * these members, so their declaration/construction order matters.
 */
class webApp
{
public:
    /**
     * @brief Construct the server and bind its controller dependencies.
     */
    webApp();
    /**
     * @brief Initialize hardware, filesystem, Wi-Fi, and routes; report success.
     */
    bool begin();
    /**
     * @brief Service HTTP and nonblocking hardware activity.
     */
    void update();

private:
    // Declaration order ensures dependencies exist before the controller.
    WebServer server;
    ledBlinker blinker;
    dacGenerator generator;
    s3Monitor monitor;
    webController controller;
    // Gate HTTP servicing after startup failure; serial monitoring still runs.
    bool serverStarted = false;
};
