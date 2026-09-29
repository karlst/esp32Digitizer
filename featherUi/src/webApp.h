/**
 * @file webApp.h
 * @brief Application ownership and startup for the Feather UI.
 */
#pragma once
#include <WebServer.h>
#include "ledBlinker.h"
#include "webController.h"

/**
 * @brief Own and coordinate the HTTP server, controller, and hardware services.
 */
class webApp
{
public:
    /** @brief Construct the server and bind its controller dependencies. */
    webApp();
    /** @brief Initialize hardware, filesystem, Wi-Fi, and routes; report success. */
    bool begin();
    /** @brief Service HTTP and nonblocking hardware activity. */
    void update();

private:
    // Declaration order ensures dependencies exist before the controller.
    WebServer server;
    ledBlinker blinker;
    webController controller;
    bool serverStarted = false;
};
