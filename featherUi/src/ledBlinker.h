/**
 * @file ledBlinker.h
 * @brief Nonblocking GPIO 13 LED control.
 */
#pragma once
#include <Arduino.h>

/**
 * @brief Own LED state and timing independently of HTTP requests.
 */
class ledBlinker
{
public:
    /** @brief Configure the LED in its stopped, off state. */
    void begin();
    /** @brief Apply any LED transition due at the current time. */
    void update();
    /** @brief Start or stop blinking; repeated commands preserve timing. */
    void setEnabled(bool enabled);
    /** @brief Report commanded blinking, regardless of current LED level. */
    bool isEnabled() const;

private:
    // Toggle every half second to produce one complete blink per second.
    static constexpr uint8_t ledPin = 13;
    static constexpr unsigned long blinkIntervalMs = 500;
    bool blinkEnabled = false;
    bool ledOn = false;
    unsigned long lastToggleMs = 0;
};
