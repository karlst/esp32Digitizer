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
 * @file Arduino.h
 * @brief Desktop-only hardware substitutes; never part of a firmware build.
 * The native test runner places this directory ahead of real Arduino headers.
 * Time advances only when a fake delay or a test changes it. Serial bytes live
 * in memory queues, and GPIO levels are test variables. There is no physical
 * board access, scheduling or electrical timing guarantee in these substitutes.
 */
#pragma once
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

/** @brief Pretend to be a serial port using memory rather than a Windows device.
 * input supplies incoming characters, output records transmitted text, capacity
 * simulates available transmit space, and flushed records whether flush was called.
 * Tests inspect these to distinguish attempted commands from completed replies. */
struct fakeSerial
{
    std::string output;
    std::deque<char> input;
    int capacity = 512;
    bool flushed = false;

    /**
     * @brief Ignore receive-buffer sizing; input is an unbounded test deque.
     */
    void setRxBufferSize(int) {}

    /**
     * @brief Ignore transmit-buffer sizing; tests set capacity directly.
     */
    void setTxBufferSize(int) {}

    /**
     * @brief Simulate UART startup.
     */
    void begin(int, int, int rx, int) { if (rx == 18) { testUartRxMapped = true; } }

    /**
     * @brief Return pending input size.
     */
    int available() { return static_cast<int>(input.size()); }

    /**
     * @brief Consume one queued character; caller must first check available().
     */
    int read() { const int retVal = input.front(); input.pop_front(); return retVal; }

    /**
     * @brief Return configured transmit capacity.
     */
    int availableForWrite() { return capacity; }

    /**
     * @brief Append all bytes to output; capacity is not enforced by this fake.
     */
    size_t write(const uint8_t* bytes, size_t count)
    { output.append(reinterpret_cast<const char*>(bytes), count); return count; }

    /**
     * @brief Remember that flush was requested; no physical transmission occurs.
     */
    void flush() { flushed = true; }

    /**
     * @brief Capture startup debug text from the Feather monitor test.
     */
    void println(const char* text) { output += text; output += '\n'; }

    /** @brief Capture formatted diagnostic output without a physical USB port. */
    template<typename... Args>
    size_t printf(const char* format, Args... args)
    {
        char text[1024];
        const int count = std::snprintf(text, sizeof(text), format, args...);
        const size_t retVal = count > 0 ? static_cast<size_t>(count) : 0;
        output.append(text, retVal < sizeof(text) ? retVal : sizeof(text) - 1);
        return retVal;
    }
};

/**
 * @brief Observe reboot without restarting the test process.
 */
struct fakeEsp
{
    int restarts = 0;

    /**
     * @brief Record a restart request.
     */
    void restart() { ++restarts; }
};
extern fakeSerial Serial;
extern fakeSerial Serial1;
extern fakeEsp ESP;
