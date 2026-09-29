/**
 * @file webApp.cpp
 * @brief Filesystem, access-point, and application lifecycle.
 */
#include <LittleFS.h>
#include <WiFi.h>
#include "webApp.h"

/** @brief Create the port-80 server and connect the controller to its server and LED service. */
webApp::webApp() : server(80), controller(server, blinker)
{
    // Defer hardware initialization until Arduino calls setup.
}

/** @brief Initialize the LED, filesystem, access point, and HTTP routes; report startup success. */
bool webApp::begin()
{
    // Initialize a safe LED state even when later startup steps fail.
    bool retVal = false;
    blinker.begin();

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

/** @brief Process pending HTTP requests and advance LED timing on each Arduino loop pass. */
void webApp::update()
{
    // Service HTTP only after startup, while always advancing hardware activity.
    if (serverStarted)
    {
        server.handleClient();
    }
    blinker.update();
}
