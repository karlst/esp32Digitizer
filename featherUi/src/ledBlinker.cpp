/**
 * @file ledBlinker.cpp
 * @brief Retained Feather LED test, controlled by legacy /api/blink routes.
 * The current browser page no longer has a blink button. This service still
 * initializes with blinking off and is updated from webApp in Arduino loop().
 * It uses elapsed time, not a sleeping task or interrupt, so it cannot hold up
 * browser handling by waiting half a second for the next transition.
 */
#include "ledBlinker.h"

/**
 * @brief Prepare GPIO 13 as an output and establish the stopped/off state.
 *
 * Write LOW before selecting output mode to start with the intended pin level.
 * Record the current time so a future Start has a known timing origin.
 */
void ledBlinker::begin()
{
    // Boot with the output low and blinking disabled.
    digitalWrite(ledPin, LOW);
    pinMode(ledPin, OUTPUT);
    blinkEnabled = false;
    ledOn = false;
    lastToggleMs = millis();
}

/**
 * @brief Change the LED level if blinking is enabled and half a second has elapsed.
 *
 * Called frequently from webApp::update(), not by a timer interrupt. Return at once
 * if nothing is due. If the web loop was delayed, toggle once at the current time
 * instead of replaying many overdue flashes. Unsigned elapsed-time subtraction
 * continues to work when the milliseconds-since-boot counter wraps.
 */
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

/**
 * @brief Start with the LED immediately on, or stop with it immediately off.
 * @param enabled Whether repeated blinking is requested.
 *
 * Repeated requests for the existing state leave timing unchanged. All callers
 * and update() share the Arduino loop task, so this state needs no mutex.
 */
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

/**
 * @brief Return whether blinking was requested, not the current light level.
 * @return True throughout a blinking run, including its half-second off periods.
 */
bool ledBlinker::isEnabled() const
{
    // Expose the commanded state to the controller.
    const bool retVal = blinkEnabled;
    return retVal;
}
