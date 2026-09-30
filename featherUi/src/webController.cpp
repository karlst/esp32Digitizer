/**
 * @file webController.cpp
 * @brief Translate browser HTTP requests into Feather actions or queued S3 commands.
 *
 * webApp registers these routes once, then server.handleClient() calls their
 * handlers from Arduino loop(). GET reads status; POST requests a change. JSON
 * is the text object returned to JavaScript. HTTP 200 means a successful reply,
 * 202 means accepted/pending, and 4xx/5xx report rejected/unavailable requests.
 *
 * A local DAC Start returns its actual state after the operation. An S3 command
 * returns pending and is confirmed later by an S3 serial acknowledgement. The
 * browser must not interpret acceptance for delivery as hardware confirmation.
 */
#include <LittleFS.h>
#include "webController.h"
#include <cmath>
#include <cstdlib>

/**
 * @brief Save references to the server and services owned by webApp.
 *
 * All referenced objects live as long as this controller. Route callbacks capture
 * this pointer to reach them later; no hardware is configured in this constructor.
 */
webController::webController(WebServer& server, ledBlinker& blinker, dacGenerator& generator, s3Monitor& monitor)
    : server(server), blinker(blinker), generator(generator), monitor(monitor)
{
    // Share services without copying their state.
}

/**
 * @brief Register the URL handlers the browser can call after the web server starts.
 *
 * server.on stores callbacks; it does not execute those callbacks here. [this]
 * lets each later callback access this controller's services. Static files are
 * served from flash, while /api routes read live state or request actions.
 * Legacy blink routes remain available even though the current page has no button.
 */
void webController::begin()
{
    // Register handlers now; handleClient() serves matching requests later in loop().
    server.on("/", HTTP_GET, [this]() { serveIndex(); });
    // Map /static/ URLs to LittleFS files, inferring MIME types from their extensions.
    // Assets are replaced independently of firmware. Revalidate them after an
    // upload so an old cached stylesheet cannot make enabled buttons look grey.
    server.serveStatic("/static/", LittleFS, "/static/", "no-cache");

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
    server.on("/api/s3/delete", HTTP_POST, [this]() { commandS3("delete"); });
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

/**
 * @brief Complete a previously requested Feather reboot after a 750 ms grace period.
 *
 * The request handler already replied to the browser. Waiting outside that handler
 * lets the reply leave before restarting, although delivery is not guaranteed.
 * Stop the DAC before restart; startup will restore defaults with output disabled.
 */
void webController::update()
{
    if (rebootPending && millis() - rebootRequestedMs >= 750)
    {
        generator.setEnabled(false);
        ESP.restart();
    }
}

/**
 * @brief Reply with one consistent snapshot of actual DAC settings and loopback samples.
 *
 * The generator copies shared data under its own lock and releases it before
 * formatting JSON. Network sending here does not hold that sampling lock.
 * no-store tells browser caches to fetch current status rather than reuse an old reply.
 */
void webController::sendDacState()
{
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", generator.stateJson());
}

/**
 * @brief Convert a required HTTP form field to a finite floating-point number.
 * @param name Field name, such as frequencyHz or sampleRate.
 * @param value Parsed result; use it only if this function returns true.
 * @return False for a missing field, no numeric characters, trailing junk, infinity,
 * or NaN (not a number). Range and integer-only checks belong to the caller.
 *
 * strtof handles numeric syntax, which is broader than our browser editor (for
 * example, it can accept a leading sign or exponent). This helper alone does not
 * establish that a number is an allowed hardware setting.
 */
bool webController::readNumber(const char* name, float& value)
{
    bool retVal = false;
    if (server.hasArg(name))
    {
        const String text = server.arg(name);
        char* end = nullptr;
        // end points to the first unparsed character. Require at least one parsed
        // character AND the end of the string, preventing "10junk" becoming 10.
        value = strtof(text.c_str(), &end);
        retVal = end != text.c_str() && *end == '\0' && std::isfinite(value);
    }
    return retVal;
}

/**
 * @brief Validate all three submitted sine settings and start the local DAC together.
 *
 * Frequency is cycles/second, amplitude is peak volts around the offset, and offset
 * is the center voltage. generator.start enforces the allowed combined voltage
 * range and rejects changing a running waveform. Invalid input leaves settings
 * unchanged and returns HTTP 400; success returns the confirmed state as JSON.
 */
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
 * @brief Validate a browser request and ask the serial monitor to send it to S3.
 * @param action One of the registered actions: start, stop, reboot, or delete.
 *
 * Only Start reads sampleRate and record; Stop/Reboot ignore invalid drafts.
 * Delete requires the browser's explicit confirmation field and stopped S3 state.
 * The monitor checks supported rates, connection freshness, and pending commands.
 * HTTP 202 means the command was sent and is awaiting S3 confirmation. HTTP 409
 * means it was not accepted for sending. This handler never waits for the S3 reply.
 */
void webController::commandS3(const char* action)
{
    float rate = 0;
    // A recording request must be explicit 0/1, never a loosely parsed truthy
    // string. Deletion requires a separate confirmation field from the browser.
    const bool isStart = strcmp(action, "start") == 0;
    const bool record = isStart && server.hasArg("record") && server.arg("record") == "1";
    const bool validRecord = !isStart || !server.hasArg("record") ||
        server.arg("record") == "0" || server.arg("record") == "1";
    const bool confirmed = strcmp(action, "delete") != 0 || server.arg("confirm") == "delete-recordings";
    const bool valid = strcmp(action, "start") != 0 ||
        (readNumber("sampleRate", rate) && rate >= 0 && rate <= 30000 && floorf(rate) == rate);
    server.sendHeader("Cache-Control", "no-store");
    if (valid && validRecord && confirmed && monitor.sendCommand(action, static_cast<uint32_t>(rate), record))
    {
        server.send(202, "application/json", monitor.stateJson());
    }
    else
    {
        server.send(409, "application/json", "{\"error\":\"S3 unavailable, busy, or command/settings invalid.\"}");
    }
}

/**
 * @brief Stream the saved HTML page from LittleFS, or return 404 if it is absent.
 *
 * LittleFS is the flash filesystem uploaded separately from firmware. Opening for
 * reading does not create the file. Close after streaming so the handle is released.
 * A missing page is an asset-upload problem, not evidence that acquisition failed.
 */
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

/**
 * @brief Reply with the legacy LED test's commanded enabled flag.
 *
 * This describes whether blinking is requested, not whether the LED is lit at this
 * instant. The current page does not use this route; it remains available for older
 * clients. Disable response caching so successive queries see actual command state.
 */
void webController::sendBlinkState()
{
    // Prevent caches from returning stale hardware command state.
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json",
        blinker.isEnabled() ? "{\"enabled\":true}" : "{\"enabled\":false}");
}
