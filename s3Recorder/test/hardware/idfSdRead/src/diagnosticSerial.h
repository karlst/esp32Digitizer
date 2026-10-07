/**
 * @file diagnosticSerial.h
 * @brief USB-bridge UART logging for the isolated ESP-IDF comparison.
 */
#pragma once
#include <cstddef>

/**
 * @brief Implement the small Serial API used by the shared diagnostic source.
 * Only the main task calls these methods. UART0 reaches COM4 through the board's
 * USB bridge; UART1 and the Feather are never started. This is not an SD adapter.
 */
class diagnosticSerial
{
public:
    void begin(unsigned long baud);
    void printf(const char* format, ...);
    void print(const char* message);
    void println(const char* message = "");
    int available();
    int read();
};
