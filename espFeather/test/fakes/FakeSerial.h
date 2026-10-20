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

/** @file FakeSerial.h
 * @brief FakeSerial declaration shared by its callers.
 */
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <deque>
#include <cstdio>
extern bool testUartRxMapped;
/** @brief Pretend to be a serial port using memory rather than a Windows device.
 * input supplies incoming characters, output records transmitted text, capacity
 * simulates available transmit space, and flushed records whether flush was called.
 * Tests inspect these to distinguish attempted commands from completed replies. */
struct FakeSerial
{
    std::string _output;
    std::deque<char> _input;
    int _capacity = 512;
    bool _flushed = false;

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
    int available() { return static_cast<int>(_input.size()); }

    /**
     * @brief Consume one queued character; caller must first check available().
     */
    int read() { const int retVal = _input.front(); _input.pop_front(); return retVal; }

    /**
     * @brief Return configured transmit capacity.
     */
    int availableForWrite() { return _capacity; }

    /**
     * @brief Append all bytes to output; capacity is not enforced by this fake.
     */
    size_t write(const uint8_t* bytes, size_t count)
    { _output.append(reinterpret_cast<const char*>(bytes), count); return count; }

    /**
     * @brief Remember that flush was requested; no physical transmission occurs.
     */
    void flush() { _flushed = true; }

    /**
     * @brief Capture startup debug text from the Feather monitor test.
     */
    void println(const char* text) { _output += text; _output += '\n'; }

    /** @brief Capture formatted diagnostic output without a physical USB port. */
    template<typename... Args>
    size_t printf(const char* format, Args... args)
    {
        char text[1024];
        const int count = std::snprintf(text, sizeof(text), format, args...);
        const size_t retVal = count > 0 ? static_cast<size_t>(count) : 0;
        _output.append(text, retVal < sizeof(text) ? retVal : sizeof(text) - 1);
        return retVal;
    }
};
