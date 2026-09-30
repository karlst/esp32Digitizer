/**
 * @file webController.cpp
 * @brief Page serving, DAC/ADC snapshots, hardware controls, and deferred reboot.
 */
#include <LittleFS.h>
#include "webController.h"
#include <cmath>
#include <cstdlib>

/** @brief Retain the server, LED, DAC/ADC, and S3 monitor services owned by webApp. */
webController::webController(WebServer& server, ledBlinker& blinker, dacGenerator& generator, s3Monitor& monitor)
    : server(server), blinker(blinker), generator(generator), monitor(monitor)
{
    // Share services without copying their state.
}

/** @brief Register page, asset, LED, DAC, S3 status, and reboot handlers during startup. */
void webController::begin()
{
    // Register handlers now; handleClient() serves matching requests later in loop().
    server.on("/", HTTP_GET, [this]() { serveIndex(); });
    // Map /static/ URLs to LittleFS files, inferring MIME types from their extensions.
    server.serveStatic("/static/", LittleFS, "/static/");

    // Telemetry reports link freshness separately from ADC sample progress. Both
    // this callback and monitor.update() run in loop(), so no extra mutex is needed.
    server.on("/api/s3", HTTP_GET, [this]()
    {
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "application/json", monitor.stateJson());
    });

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
    server.on("/api/dac/start", HTTP_POST, [this]() { startDac(); });
    // A 202 response means sent, not applied. Status polls report the matched S3 acknowledgement.
    server.on("/api/s3/start", HTTP_POST, [this]() { commandS3("start"); });
    server.on("/api/s3/stop", HTTP_POST, [this]() { commandS3("stop"); });
    server.on("/api/s3/reboot", HTTP_POST, [this]() { commandS3("reboot"); });
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

/** @brief Start with validated sine settings, preserving current state on rejection. */
void webController::startDac()
{
    float frequency = 0;
    float amplitude = 0;
    float offset = 0;
    // Firmware validation remains authoritative even for clients bypassing the UI.
    if (readNumber("frequencyHz", frequency) && readNumber("amplitudeVolts", amplitude) &&
        readNumber("offsetVolts", offset) && generator.start(frequency, amplitude, offset))
    {
        sendDacState();
    }
    else
    {
        server.send(400, "application/json", "{\"error\":\"Start rejected. Check settings and stop DAC before changing them.\"}");
    }
}

/**
 * @brief Send a validated command without waiting in the HTTP handler for UART traffic.
 * Stop/Reboot do not read settings. This keeps Stop usable even if a browser draft
 * is invalid. A command succeeds only when s3Monitor matches its id to an S3 reply.
 */
void webController::commandS3(const char* action)
{
    float rate = 0;
    const bool valid = strcmp(action, "start") != 0 ||
        (readNumber("sampleRate", rate) && rate >= 0 && rate <= 30000 && floorf(rate) == rate);
    server.sendHeader("Cache-Control", "no-store");
    if (valid && monitor.sendCommand(action, static_cast<uint32_t>(rate)))
    {
        server.send(202, "application/json", monitor.stateJson());
    }
    else
    {
        server.send(409, "application/json", "{\"error\":\"S3 unavailable, busy, or command/settings invalid.\"}");
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
