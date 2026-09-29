/**
 * @file webController.h
 * @brief HTTP routes for page delivery and hardware commands.
 */
#pragma once
#include <WebServer.h>
#include "ledBlinker.h"

/**
 * @brief Translate HTTP requests into hardware commands and state responses.
 */
class webController
{
public:
    /** @brief Bind to services owned by the application. */
    webController(WebServer& server, ledBlinker& blinker);
    /** @brief Register page, asset, and API routes before server startup. */
    void begin();

private:
    /** @brief Stream the landing page or report a missing asset. */
    void serveIndex();
    /** @brief Respond with authoritative blink state as uncached JSON. */
    void sendBlinkState();

    // The application owns these services and outlives route callbacks.
    WebServer& server;
    ledBlinker& blinker;
};
