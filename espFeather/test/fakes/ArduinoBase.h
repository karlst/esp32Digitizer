/*
 * Copyright (C) 2026 Karl Stock
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of ESP32 Digitizer.
 * You may redistribute and/or modify it under the GNU General Public License
 * as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 *
 * Distributed WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License in the repository LICENSE file or <https://www.gnu.org/licenses/>.
 */

/**
 * @file ArduinoBase.h
 * @brief Desktop-only hardware substitutes; never part of a firmware build.
 * The native test runner places this directory ahead of real Arduino headers.
 * Time advances only when a fake delay or a test changes it. Serial bytes live
 * in memory queues, and GPIO levels are test variables. There is no physical
 * board access, scheduling or electrical timing guarantee in these substitutes.
 */
#pragma once
#include "FakeEsp.h"
#include "FakeSerial.h"
#include <cstdint>
#include <cstddef>
#include <string>
#include <deque>
#include <cstdio>
#define IRAM_ATTR
#define LOW 0
#define HIGH 1
#define INPUT_PULLUP 2
#define FALLING 3
#define SERIAL_8N1 0
#define OUTPUT 4
extern uint32_t testMs;
extern uint32_t testUs;
extern int testReady;
extern int testChipSelect;

// Model the S3 native UART RX mux: ordinary GPIO configuration removes it.
// Shared inline variables let test and production translation units observe the
// same simulated state. UINT32_MAX here means no scheduled DRDY rise.
inline bool testUartRxMapped = false;
inline uint32_t testReadyRiseAt = UINT32_MAX;

// Optional hardware simulation tick; ordinary tests leave time under manual control.
inline void (*testClockHook)() = nullptr;

/**
 * @brief Return the test-controlled millisecond counter without advancing it.
 */
inline uint32_t millis() { return testMs; }

/**
 * @brief Run the optional hardware-simulation hook, then return test microseconds.
 */
inline uint32_t micros() { if (testClockHook) { testClockHook(); } return testUs; }

/**
 * @brief Return testReady for every pin; GPIO9 may also trigger a scheduled rise.
 * This is a DRDY-focused substitute, not an independent value for every GPIO.
 */
inline int digitalRead(int pin)
{
    if (pin == 9 && testReadyRiseAt != UINT32_MAX && testUs >= testReadyRiseAt)
    {
        testReady = HIGH;
        testReadyRiseAt = UINT32_MAX;
    }
    return testReady;
}

/**
 * @brief Model the native UART pin being returned to ordinary GPIO mode.
 */
inline void pinMode(int pin, int) { if (pin == 18) { testUartRxMapped = false; } }

/**
 * @brief Save any digitalWrite value as testChipSelect; the pin argument is ignored.
 */
inline void digitalWrite(int, int value) { testChipSelect = value; }

/**
 * @brief Advance only testUs by duration microseconds; do not sleep the PC thread.
 */
inline void delayMicroseconds(uint32_t duration) { testUs += duration; }

/**
 * @brief Advance only testMs by duration milliseconds; testUs is a separate clock.
 */
inline void delay(uint32_t duration) { testMs += duration; }

/**
 * @brief Interrupt registration is driven manually by the tests.
 */
inline void attachInterruptArg(int, void (*)(void*), void*, int) {}

/**
 * @brief Interrupt removal is tracked by firmware state in these tests.
 */
inline void detachInterrupt(int) {}




extern FakeSerial Serial;
extern FakeSerial Serial1;
extern FakeEsp ESP;
