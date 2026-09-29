/**
 * @file main.cpp
 * @brief Arduino entry points for the Feather web application.
 */
#include <Arduino.h>
#include <version.hpp>
#include "webApp.h"

// Keep the application alive for the lifetime of the firmware.
static webApp app;

/**
 * @brief Initialize diagnostics and application services.
 */
void setup()
{
    // Report firmware identity before starting the application.
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("ESP32 Digitizer Feather UI");
    Serial.print("Version: ");
    Serial.println(ESP32_DIGITIZER_VERSION);

    // Startup failures are reported by the application through serial diagnostics.
    app.begin();
}

/**
 * @brief Service HTTP and hardware without blocking on blink timing.
 */
void loop()
{
    // Advance all application services together.
    app.update();
}
