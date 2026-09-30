/**
 * @file main.cpp
 * @brief Starting point for the Feather firmware, separate from the S3 firmware.
 * Arduino calls setup() once and then loop() repeatedly. webApp performs the
 * actual service startup/updates; dacGenerator creates its own background task.
 * Read webApp.cpp next for ownership and startup, webController.cpp for routes.
 */
#include <Arduino.h>
#include <version.hpp>
#include "webApp.h"

// Keep the application alive for the lifetime of the firmware.
static webApp app;

/**
 * @brief Open USB debug output, print the firmware version, and start the application.
 *
 * The initial half-second delay allows startup output to settle; there is no loop
 * waiting for a USB terminal to connect. The application therefore also starts when
 * powered from the other board. Service failures are reported by webApp and status.
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
 * @brief Let webApp process the next available serial message or browser request.
 *
 * Arduino repeatedly calls this function; it is not a new thread on each call.
 * LED timing checks do not sleep, and the DAC timing task runs separately. This
 * loop is not responsible for sampling the S3 digitizer.
 */
void loop()
{
    // Advance all application services together.
    app.update();
}
