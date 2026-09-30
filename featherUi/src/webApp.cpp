/**
 * @file webApp.cpp
 * @brief Bring up the Feather services and keep them running from Arduino loop().
 *
 * main.cpp owns one webApp. It owns the HTTP server, local DAC generator, legacy
 * LED service, S3 serial monitor, and the controller that connects browser routes
 * to those services. Browser files live in LittleFS, a separate flash filesystem.
 * Uploading firmware alone does not update those HTML/JavaScript/CSS files.
 */
#include <LittleFS.h>
#include <WiFi.h>
#include "webApp.h"

/**
 * @brief Connect the application objects without starting hardware during construction.
 *
 * server(80) listens on the standard HTTP port. monitor(Serial1) uses the physical
 * serial connection to S3. controller receives references to these existing objects;
 * it does not make copies. Declaration order in webApp.h creates dependencies first.
 */
webApp::webApp() : server(80), monitor(Serial1), controller(server, blinker, generator, monitor)
{
    // Defer hardware initialization until Arduino calls setup.
}

/**
 * @brief Start hardware services, mount browser files, create Wi-Fi, and open HTTP.
 * @return True if the filesystem mounted and the access point/web server started.
 * This does not prove DAC initialization succeeded or an S3 is connected.
 *
 * Called once from Arduino setup(). The Feather creates its own Wi-Fi access point
 * named ESP32-Digitizer; it does not join a home router. The UI remains available
 * if DAC initialization fails so it can show the fault and offer reboot.
 */
bool webApp::begin()
{
    // Keep outputs disabled at startup; expose the UI even if analog startup fails.
    bool retVal = false;
    blinker.begin();
    generator.begin();
    monitor.begin();

    // LittleFS is the web-file storage in flash. true allows formatting if mounting
    // fails; that can leave an empty filesystem that needs the web files uploaded.
    // A successful mount does not prove index.html exists (the controller checks).
    if (!LittleFS.begin(true))
    {
        Serial.println("LittleFS mount failed.");
    }
    else
    {
        // AP means access point. With no password argument this is an open local
        // network. Its address is printed below for the browser to connect to.
        WiFi.mode(WIFI_AP);
        if (!WiFi.softAP("ESP32-Digitizer"))
        {
            Serial.println("Wi-Fi access point start failed.");
        }
        else
        {
            // Register handlers before accepting requests.
            controller.begin();
            server.begin();
            serverStarted = true;
            retVal = true;

            // Report connection details for the browser client.
            Serial.println("Access point: ESP32-Digitizer");
            Serial.print("Access point IP: ");
            Serial.println(WiFi.softAPIP());
            Serial.println("Web server started.");
        }
    }
    return retVal;
}

/**
 * @brief Service serial messages, browser requests, LED timing, and deferred reboot.
 *
 * main.cpp calls this on every Arduino loop pass. These services share that task,
 * so the monitor and web controller cannot change monitor state simultaneously.
 * The DAC has its own worker and mutex; it does not depend on this loop for ticks.
 * S3 input is drained even if web startup failed. HTTP is served only after success.
 */
void webApp::update()
{
    // Drain S3 telemetry in this task; the independent DAC worker never accesses it.
    monitor.update();
    // Service HTTP only after startup, while always advancing hardware activity.
    if (serverStarted)
    {
        server.handleClient();
    }
    blinker.update();
    controller.update();
}
