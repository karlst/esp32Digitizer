/**
 * @file main.cpp
 * @brief Minimal Feather-hosted web UI placeholder for the ESP32 Digitizer project.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>

#include <version.hpp>

static const char* pszAccessPointName = "ESP32-Digitizer";

WebServer gWebServer(80);

/**
 * @brief Configure filesystem, access point, and HTTP server.
 */
void setup()
{
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("ESP32 Digitizer Feather UI");
    Serial.print("Version: ");
    Serial.println(ESP32_DIGITIZER_VERSION);

    bool bFsMounted = LittleFS.begin(true);

    if (!bFsMounted)
    {
        Serial.println("LittleFS mount failed.");
    }
    else
    {
        WiFi.mode(WIFI_AP);

        bool bApStarted = WiFi.softAP(pszAccessPointName);

        if (!bApStarted)
        {
            Serial.println("Wi-Fi access point start failed.");
        }
        else
        {
            Serial.print("Access point: ");
            Serial.println(pszAccessPointName);

            Serial.print("Access point IP: ");
            Serial.println(WiFi.softAPIP());

            gWebServer.on("/", HTTP_GET, []()
            {
                File tFile = LittleFS.open("/index.html", "r");

                if (tFile)
                {
                    gWebServer.streamFile(tFile, "text/html");
                    tFile.close();
                }
                else
                {
                    gWebServer.send(404, "text/plain", "index.html not found");
                }
});

gWebServer.serveStatic("/app.css", LittleFS, "/app.css", "text/css");
gWebServer.serveStatic("/app.js", LittleFS, "/app.js", "application/javascript");

            gWebServer.begin();

            Serial.println("Web server started.");
        }
    }
}

/**
 * @brief Service HTTP requests.
 */
void loop()
{
    gWebServer.handleClient();
}
