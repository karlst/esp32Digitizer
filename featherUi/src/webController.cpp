/**
 * @file webController.cpp
 * @brief Page serving, DAC/ADC snapshots, hardware controls, and deferred reboot.
 */
#include <LittleFS.h>
#include "webController.h"
#include <cmath>
#include <cstdlib>

/** @brief Retain references to the server, LED service, and DAC/ADC generator. */
webController::webController(WebServer& server, ledBlinker& blinker, dacGenerator& generator)
    : server(server), blinker(blinker), generator(generator)
{
    // Share services without copying their state.
}

/** @brief Register page, asset, LED, DAC, and reboot handlers during startup. */
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

    // DAC state is authoritative; POST routes change settings or explicit output state.
    server.on("/api/dac", HTTP_GET, [this]() { sendDacState(); });
    server.on("/api/dac/settings", HTTP_POST, [this]() { configureDac(); });
    server.on("/api/dac/start", HTTP_POST, [this]()
    {
        // Report initialization failure rather than acknowledging an unapplied command.
        if (generator.setEnabled(true))
        {
            sendDacState();
        }
        else
        {
            server.send(503, "application/json", "{\"error\":\"DAC unavailable. Try rebooting the Feather.\"}");
        }
    });
    server.on("/api/dac/stop", HTTP_POST, [this]()
    {
        // A successful response confirms that code zero has been written to the DAC.
        if (generator.setEnabled(false))
        {
            sendDacState();
        }
        else
        {
            server.send(503, "application/json", "{\"error\":\"DAC unavailable. Try rebooting the Feather.\"}");
        }
    });
    // Acknowledge before scheduling a restart; the browser handles reconnecting.
    server.on("/api/reboot", HTTP_POST, [this]()
    {
        server.sendHeader("Cache-Control", "no-store");
        server.send(202, "application/json", "{\"rebooting\":true}");
        rebootRequestedMs = millis();
        rebootPending = true;
    });
}

/** @brief Restart outside the request callback, after a short response-delivery grace period. */
void webController::update()
{
    if (rebootPending && millis() - rebootRequestedMs >= 750)
    {
        generator.setEnabled(false);
        ESP.restart();
    }
}

/** @brief Return the latest coherent DAC and ADC snapshot without browser caching. */
void webController::sendDacState()
{
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", generator.stateJson());
}

/** @brief Accept a complete finite numeric field, rejecting missing or malformed values. */
bool webController::readNumber(const char* name, float& value)
{
    bool retVal = false;
    if (server.hasArg(name))
    {
        const String text = server.arg(name);
        char* end = nullptr;
        value = strtof(text.c_str(), &end);
        retVal = end != text.c_str() && *end == '\0' && std::isfinite(value);
    }
    return retVal;
}

/** @brief Apply validated sine settings or preserve the existing settings on rejection. */
void webController::configureDac()
{
    float frequency = 0;
    float amplitude = 0;
    float offset = 0;
    // Firmware validation remains authoritative even for clients bypassing the UI.
    if (readNumber("frequencyHz", frequency) && readNumber("amplitudeVolts", amplitude) &&
        readNumber("offsetVolts", offset) && generator.configure(frequency, amplitude, offset))
    {
        sendDacState();
    }
    else
    {
        server.send(400, "application/json", "{\"error\":\"Invalid settings or DAC unavailable. Check frequency and offset +/- amplitude limits.\"}");
    }
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
