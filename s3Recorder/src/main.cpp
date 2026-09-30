/**
 * @file main.cpp
 * @brief Arduino startup and main loop for the S3 board.
 *
 * setup() runs once. It starts serial links and creates the acquisition task,
 * but never starts sample collection automatically. loop() repeatedly services
 * communications on Arduino's configured core (core 1 in our build). The task
 * created by recorder.begin() handles digitizer hardware separately on core 0.
 * Read next: acquisition.cpp for sample collection, featherLink.cpp for commands.
 */
#include <Arduino.h>
#include "acquisition.h"
#include "featherLink.h"

// Global objects live until reboot; their constructors do not start hardware.
// controlLink keeps a reference to recorder, so recorder must be constructed first.
static acquisition recorder;
static featherLink controlLink(recorder);

/**
 * @brief Open USB diagnostics and the Feather serial link, then create acquisition.
 *
 * Arduino calls this once on startup. Serial is the USB-connected debug port;
 * controlLink uses Serial1 on pins 18/17 to reach Feather. Larger transmit buffers
 * allow full status lines without waiting for individual characters to leave.
 * Digitizer initialization runs in the new task and reports its result in status.
 */
void setup()
{
    Serial.setTxBufferSize(512);
    Serial.begin(115200);
    controlLink.begin();
    // Do not wait for a USB terminal: external-power operation must work too.
    recorder.begin();
    Serial.println("S3 recorder: AIN0-AIN1, gain 1; Feather UART RX18/TX17; starts stopped.");
}

/**
 * @brief Repeatedly service Feather/USB commands and outgoing status on core 1.
 *
 * Arduino calls this again whenever it returns. A one-millisecond delay yields
 * CPU time to other tasks; it does not set the digitizer's sample rate. The
 * independent acquisition task waits on the digitizer's Data Ready signal.
 */
void loop()
{
    controlLink.update();
    // UART buffers incoming commands during this short scheduler yield.
    delay(1);
}
