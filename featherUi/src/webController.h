/**
 * @file webController.h
 * @brief HTTP routes for page delivery and hardware commands.
 */
#pragma once
#include <WebServer.h>
#include "ledBlinker.h"
#include "dacGenerator.h"
#include "s3Monitor.h"

/**
 * @brief Translate HTTP requests into hardware commands and state responses.
 */
class webController
{
public:
    /** @brief Bind to services owned by the application. */
    webController(WebServer& server, ledBlinker& blinker, dacGenerator& generator, s3Monitor& monitor);
    /** @brief Register page, asset, and API routes before server startup. */
    void begin();
    /** @brief Execute an acknowledged reboot after allowing the response to leave. */
    void update();

private:
    /** @brief Stream the landing page or report a missing asset. */
    void serveIndex();
    /** @brief Respond with authoritative blink state as uncached JSON. */
    void sendBlinkState();
    /** @brief Send settings and timestamped DAC/ADC graph data. */
    void sendDacState();
    /** @brief Validate submitted settings and start output atomically. */
    void startDac();
    /** @brief Validate an S3 command and queue it for UART acknowledgement. */
    void commandS3(const char* action);
    /** @brief Parse a required finite decimal form parameter without accepting trailing junk. */
    bool readNumber(const char* name, float& value);

    // The application owns these services and outlives route callbacks.
    WebServer& server;
    ledBlinker& blinker;
    dacGenerator& generator;
    s3Monitor& monitor;
    bool rebootPending = false;
    unsigned long rebootRequestedMs = 0;
};
