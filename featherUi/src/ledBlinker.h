/**
 * @file ledBlinker.h
 * @brief Nonblocking GPIO 13 LED control.
 */
#pragma once
#include <Arduino.h>

/**
 * @brief Store the legacy LED test's requested state and current output level.
 * No task or timer is created here. webApp calls update() from Arduino loop(),
 * and HTTP callbacks in that same task call setEnabled(). GPIO13 is a Feather
 * pin; it is unrelated to S3 GPIO13 used for digitizer data.
 */
class ledBlinker
{
public:
    /**
     * @brief Configure the LED in its stopped, off state.
     */
    void begin();
    /**
     * @brief Apply any LED transition due at the current time.
     */
    void update();
    /**
     * @brief Start or stop blinking; repeated commands preserve timing.
     */
    void setEnabled(bool enabled);
    /**
     * @brief Report commanded blinking, regardless of current LED level.
     */
    bool isEnabled() const;

private:
    // Toggle every half second to produce one complete blink per second.
    static constexpr uint8_t ledPin = 13;
    static constexpr unsigned long blinkIntervalMs = 500;
    // Requested activity and physical level differ during each off half-cycle.
    bool blinkEnabled = false;
    bool ledOn = false;
    unsigned long lastToggleMs = 0;
};
