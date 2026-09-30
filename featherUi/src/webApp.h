/**
 * @file webApp.h
 * @brief Application ownership and startup for the Feather UI.
 */
#pragma once
#include <WebServer.h>
#include "ledBlinker.h"
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
