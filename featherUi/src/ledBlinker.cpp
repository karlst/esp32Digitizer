/**
 * @file ledBlinker.cpp
 * @brief GPIO output and elapsed-time handling for the blink test.
 */
#include "ledBlinker.h"

/** @brief Configure GPIO 13 as an output and reset blinking to the off state. */
void ledBlinker::begin()
{
    // Boot with the output low and blinking disabled.
    digitalWrite(ledPin, LOW);
    pinMode(ledPin, OUTPUT);
    blinkEnabled = false;
    ledOn = false;
    lastToggleMs = millis();
}

/** @brief Toggle the LED when its half-second interval expires, without waiting. */
void ledBlinker::update()
{
    // Unsigned subtraction handles millis rollover without special cases.
    const unsigned long nowMs = millis();
    if (blinkEnabled && nowMs - lastToggleMs >= blinkIntervalMs)
    {
        lastToggleMs = nowMs;
        ledOn = !ledOn;
        digitalWrite(ledPin, ledOn ? HIGH : LOW);
    }
}

/** @brief Start blinking immediately or stop with the LED off; ignore unchanged commands. */
void ledBlinker::setEnabled(bool enabled)
{
    // Start immediately with the LED on; stopping leaves it off.
    if (blinkEnabled != enabled)
    {
        blinkEnabled = enabled;
        ledOn = enabled;
        lastToggleMs = millis();
        digitalWrite(ledPin, ledOn ? HIGH : LOW);
    }
}

/** @brief Report whether blinking is enabled, rather than whether the LED is currently lit. */
bool ledBlinker::isEnabled() const
{
    // Expose the commanded state to the controller.
    const bool retVal = blinkEnabled;
    return retVal;
}
