/**
 * @file webController.cpp
 * @brief Page serving and explicit LED start/stop endpoints.
 */
#include <LittleFS.h>
#include "webController.h"

/** @brief Retain references to the application's server and LED service for request handling. */
webController::webController(WebServer& server, ledBlinker& blinker)
    : server(server), blinker(blinker)
{
    // Share services without copying their state.
}

/** @brief Register page, static-file, and blink API handlers during application startup. */
void webController::begin()
{
    // Register handlers now; handleClient() serves matching requests later in loop().
    server.on("/", HTTP_GET, [this]() { serveIndex(); });
    // Map /static/ URLs to LittleFS files, inferring MIME types from their extensions.
    server.serveStatic("/static/", LittleFS, "/static/");

    // State reads synchronize reloads and multiple browser clients.
    server.on("/api/blink", HTTP_GET, [this]() { sendBlinkState(); });

    // Explicit commands make retries safe without accidentally toggling state.
    server.on("/api/blink/start", HTTP_POST, [this]()
    {
        blinker.setEnabled(true);
        sendBlinkState();
    });
    server.on("/api/blink/stop", HTTP_POST, [this]()
    {
        blinker.setEnabled(false);
        sendBlinkState();
    });
}

/** @brief Send index.html from LittleFS, or return HTTP 404 when the page is missing. */
void webController::serveIndex()
{
    // Close the page after streaming, or report the missing filesystem file.
    File indexFile = LittleFS.open("/index.html", "r");
    if (indexFile)
    {
        server.streamFile(indexFile, "text/html");
        indexFile.close();
    }
    else
    {
        server.send(404, "text/plain", "index.html not found");
    }
}

/** @brief Send the current blink-enabled flag as JSON with caching disabled. */
void webController::sendBlinkState()
{
    // Prevent caches from returning stale hardware command state.
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json",
        blinker.isEnabled() ? "{\"enabled\":true}" : "{\"enabled\":false}");
}
