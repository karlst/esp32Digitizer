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
 * @brief Return simulated elapsed time.
 */
inline uint32_t millis() { return testMs; }
/**
 * @brief Return simulated SPI timing.
 */
inline uint32_t micros() { if (testClockHook) { testClockHook(); } return testUs; }
/**
 * @brief Return simulated DRDY.
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
 * @brief Observe the driver's chip-select level.
 */
inline void digitalWrite(int, int value) { testChipSelect = value; }
/**
 * @brief Record SPI command delays.
 */
inline void delayMicroseconds(uint32_t duration) { testUs += duration; }
/**
 * @brief Advance simulated time.
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
     * @brief Simulate buffer sizing.
     */
    void setRxBufferSize(int) {}
    /**
     * @brief Simulate buffer sizing.
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
     * @brief Consume one received byte.
     */
    int read() { const int retVal = input.front(); input.pop_front(); return retVal; }
    /**
     * @brief Return configured transmit capacity.
     */
    int availableForWrite() { return capacity; }
    /**
     * @brief Capture an output frame.
     */
    size_t write(const uint8_t* bytes, size_t count)
    { output.append(reinterpret_cast<const char*>(bytes), count); return count; }
    /**
     * @brief Record acknowledgement completion before reboot.
     */
    void flush() { flushed = true; }
    /**
     * @brief Capture startup debug text from the Feather monitor test.
     */
    void println(const char* text) { output += text; output += '\n'; }
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
