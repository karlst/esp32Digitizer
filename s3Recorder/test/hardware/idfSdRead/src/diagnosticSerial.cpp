/**
 * @file diagnosticSerial.cpp
 * @brief Forward diagnostic text and repeat commands through ESP-IDF UART0.
 * The SDK initializes console pins. We add an RX buffer for the R command and
 * send text through stdio. No card operations occur in this compatibility layer.
 */
#include "Arduino.h"
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_system.h>
#include <cstdio>
#include <cstdarg>
diagnosticSerial Serial;

/** @brief Install UART0 receive buffering; baud is bits per second, normally 115200. */
void diagnosticSerial::begin(unsigned long baud)
{
    ESP_ERROR_CHECK(uart_set_baudrate(UART_NUM_0, baud));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 256, 0, 0, nullptr, 0));
    std::printf("DIAG framework=%s standalone ESP-IDF\n", esp_get_idf_version());
}

/** @brief Format a diagnostic line; arguments follow standard printf conventions. */
void diagnosticSerial::printf(const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    std::vprintf(format, arguments);
    va_end(arguments);
    std::fflush(stdout);
}

/** @brief Send text without a newline, used for the diagnostic hex dumps. */
void diagnosticSerial::print(const char* message)
{
    std::fputs(message, stdout);
    std::fflush(stdout);
}

/** @brief Send text followed by a newline; no argument emits a blank line. */
void diagnosticSerial::println(const char* message)
{
    std::puts(message);
    std::fflush(stdout);
}

/** @brief Return the number of received UART bytes waiting for the main task. */
int diagnosticSerial::available()
{
    size_t length = 0;
    uart_get_buffered_data_len(UART_NUM_0, &length);
    const int retVal = static_cast<int>(length);
    return retVal;
}

/** @brief Read one buffered byte without waiting, or return -1 if none is present. */
int diagnosticSerial::read()
{
    uint8_t byte = 0;
    const int retVal = uart_read_bytes(UART_NUM_0, &byte, 1, 0) == 1 ? byte : -1;
    return retVal;
}

/** @brief Yield the main task for the requested milliseconds, preserving GPIO state. */
void delay(unsigned long milliseconds)
{
    vTaskDelay(pdMS_TO_TICKS(milliseconds));
}
