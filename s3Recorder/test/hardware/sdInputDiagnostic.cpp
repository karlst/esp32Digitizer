/**
 * @file sdInputDiagnostic.cpp
 * @brief Observe SD breakout data connections as inputs, with the card removed.
 * Build sd-input-diagnostic only. No SD controller, file access, digitizer or
 * Feather link is started. The main Arduino thread owns all observations.
 *
 * Reading path: setup() releases the three pins to high-impedance input mode;
 * reportInputs() reads their digital levels; loop() prints once per second.
 * The breakout's external pullups normally hold unconnected data pins HIGH.
 * To test one connection, use a 1-kohm resistor from its breakout pin to GND,
 * then to breakout 3V. Expect LOW then HIGH on only that named input. The
 * resistor limits current if the wrong pin or firmware is selected. Never
 * insert the SD card or attach LEDs during this test. Input levels verify a
 * slow electrical path; they do not prove high-speed SD communication works.
 */
#if S3_SD_INPUT_DIAGNOSTIC
#include <Arduino.h>

// S3 GPIO numbers corresponding to the three breakout labels, not header positions.
static const uint8_t inputPins[] = {15, 16, 4};
static constexpr size_t inputCount = sizeof(inputPins) / sizeof(inputPins[0]);

/**
 * @brief Print actual GPIO input readings; HIGH is 1 and LOW is 0.
 * digitalRead samples each pin in sequence, not simultaneously. This is adequate
 * for a manually held test lead. It is not a voltage measurement or pulse logger.
 * USB messages only report observations; they never drive the data connections.
 */
static void reportInputs()
{
    const int d1 = digitalRead(inputPins[0]);
    const int dat2 = digitalRead(inputPins[1]);
    const int d3 = digitalRead(inputPins[2]);
    Serial.printf("INPUT TEST: D1(GPIO15)=%d DAT2(GPIO16)=%d D3-CS(GPIO4)=%d [1=HIGH 0=LOW]\n",
        d1, dat2, d3);
}

/**
 * @brief Release all test pins as inputs before announcing readiness over USB.
 * INPUT disables internal pullups/pulldowns; the breakout supplies its own
 * pullups. This firmware never calls digitalWrite or enables any pin as output.
 * Wait for the ready message before connecting the resistor test lead.
 */
void setup()
{
    for (size_t index = 0; index < inputCount; ++index)
    {
        pinMode(inputPins[index], INPUT);
    }
    Serial.begin(115200);
    Serial.println("INPUT TEST READY: GPIO15/16/4 inputs only; card removed; no SD reads/writes.");
}

/**
 * @brief Sample the three inputs every second until power-off or new firmware.
 * delay yields CPU time while leaving the pins as inputs. A momentary connection
 * shorter than a second could be missed; hold each test connection for several
 * seconds while the computer captures its serial report.
 */
void loop()
{
    reportInputs();
    delay(1000);
}
#endif
