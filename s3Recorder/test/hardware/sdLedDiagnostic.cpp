/**
 * @file sdLedDiagnostic.cpp
 * @brief Slowly exercise the three added SD data connections with LEDs.
 * Use only with the SD card REMOVED. Each breakout pin feeds its own resistor
 * and LED to common ground: D1 green, DAT2 red, D3/CS white. No SD controller,
 * filesystem, ADC, or Feather UART is started by this standalone build.
 *
 * Reading path: setup() makes all three GPIOs outputs at zero volts; loop()
 * selects each LED in turn. showStep() changes the outputs, prints what should
 * be visible, and holds that state for three seconds. HIGH supplies about 3.3 V;
 * LOW holds near ground. A lit LED tests the path to that breakout connection,
 * not the card socket contacts or high-speed signal quality. A failed indication
 * could also mean reversed LED polarity, wrong wiring, or a faulty GPIO.
 */
#if S3_SD_LED_DIAGNOSTIC
#include <Arduino.h>

// These are S3 GPIO numbers, not header positions. All LEDs have separate
// series resistors; their short (negative) leads share the breakout ground.
static const uint8_t ledPins[] = {15, 16, 4};
static const char* ledNames[] = {"GREEN D1 / GPIO15", "RED DAT2 / GPIO16", "WHITE D3-CS / GPIO4"};
static constexpr size_t ledCount = sizeof(ledPins) / sizeof(ledPins[0]);

/**
 * @brief Hold one named LED on, or all off, for three seconds.
 * selected is an index into ledPins; -1 means all off. Every transition first
 * drives all outputs LOW, preventing overlap. Only Arduino's main thread calls
 * this function, so there is no shared-state lock or background worker.
 * delay() yields processor time while maintaining the pin voltages; it does not
 * generate pulses. Serial text states the commanded condition, not a measured one.
 */
static void showStep(int selected)
{
    for (size_t index = 0; index < ledCount; ++index)
    {
        digitalWrite(ledPins[index], LOW);
    }
    if (selected >= 0 && selected < static_cast<int>(ledCount))
    {
        digitalWrite(ledPins[selected], HIGH);
        Serial.printf("LED TEST: only %s ON for 3 seconds\n", ledNames[selected]);
    }
    else
    {
        Serial.println("LED TEST: ALL OFF for 3 seconds");
    }
    delay(3000);
}

/**
 * @brief Arduino startup opens USB logging and initializes the three test pins.
 * The output latch is set LOW before enabling each output to avoid a deliberate
 * startup flash. Pullups on the breakout may light LEDs before firmware starts;
 * evaluate only the repeating sequence once startup has completed.
 */
void setup()
{
    Serial.begin(115200);
    for (size_t index = 0; index < ledCount; ++index)
    {
        digitalWrite(ledPins[index], LOW);
        pinMode(ledPins[index], OUTPUT);
    }
    Serial.println("LED TEST ONLY: card must be removed; no SD reads or writes; Feather link inactive.");
}

/**
 * @brief Repeat green, red, white, each separated by a three-second all-off step.
 * Arduino calls this repeatedly. All other test LEDs should stay dark whenever
 * one is selected. Seeing two change together can indicate a crossed connection
 * or short; seeing a different color points to a mapping/wiring discrepancy.
 */
void loop()
{
    for (size_t index = 0; index < ledCount; ++index)
    {
        showStep(-1);
        showStep(static_cast<int>(index));
    }
}
#endif
