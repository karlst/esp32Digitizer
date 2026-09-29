/**
 * @file webApp.cpp
 * @brief Filesystem, access-point, and application lifecycle.
 */
#include <LittleFS.h>
#include <WiFi.h>
#include "webApp.h"

/** @brief Create the port-80 server and bind the controller to the LED and DAC/ADC services. */
webApp::webApp() : server(80), controller(server, blinker, generator)
{
    // Defer hardware initialization until Arduino calls setup.
}

/** @brief Initialize hardware, filesystem, access point, and HTTP routes; report web startup success. */
bool webApp::begin()
{
    // Keep outputs disabled at startup; expose the UI even if analog startup fails.
    bool retVal = false;
    blinker.begin();
    generator.begin();

    // Mount browser assets before exposing the access point.
    if (!LittleFS.begin(true))
    {
        Serial.println("LittleFS mount failed.");
    }
    else
    {
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

/** @brief Process HTTP, advance LED timing, and execute pending reboot requests. */
void webApp::update()
{
    // Service HTTP only after startup, while always advancing hardware activity.
    if (serverStarted)
    {
        server.handleClient();
    }
    blinker.update();
    controller.update();
}
