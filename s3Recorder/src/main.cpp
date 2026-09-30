/**
 * @file main.cpp
 * @brief S3 digitizer acquisition controlled and monitored by the Feather web UI.
 */
#include <Arduino.h>
#include "acquisition.h"
#include "featherLink.h"

static acquisition recorder;
static featherLink controlLink(recorder);

/**
 * @brief Open links and start the ADC worker; acquisition stays stopped.
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
 * @brief Service commands while the separate worker collects conversions.
 */
void loop()
{
    controlLink.update();
    // UART buffers incoming commands during this short scheduler yield.
    delay(1);
}
