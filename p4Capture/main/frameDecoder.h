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
 * @file frameDecoder.h
 * @brief Decode ADC data and its frame marker from paired PARLIO input bits.
 * Read feed() first, then beginFrame(), appendBits() and finishWord(). No hardware or RTOS
 * dependency: the same decoder is exercised by the desktop test program.
 */
#pragma once
#include <cstddef>
#include <cstdint>

/** @brief Observations of received data, not a claim that every ADC bit arrived. */
struct captureStats
{
    uint64_t clocks = 0;
    uint32_t frames = 0;
    uint32_t words = 0;
    uint32_t badHeaders = 0;
    uint32_t badPeriods = 0;
    uint32_t shortWords = 0;
    uint32_t latestWord = 0;
    int32_t latestSample = 0;
    uint32_t headers[256] = {};
    uint32_t badExamples[8] = {};
};

/**
 * @brief Worker-owned state spanning DMA blocks, including split ADC words.
 * A sample is one ADC conversion result. In our selected channel arrangement,
 * FSYNC (frame synchronization) rises once per sample; DCLK provides one clock
 * per serial bit. A rising edge means low-to-high, not every captured high level.
 * FSYNC falls halfway through its period; data continue while it is low. Each packed pair
 * contains DOUT0 in bit 0 and FSYNC in bit 1, earliest pair in byte bits 1:0.
 * Only the first 32 DCLKs of a frame form our 8-bit header + 24-bit sample.
 * Idle clocks in the rest of a frame are deliberately ignored.
 * An optional output callback receives copies of complete raw words in batches
 * of at most 512. A rejected batch latches failure but does not hide diagnostic
 * counts. reset clears that latch while preserving the selected callback.
 * The output is 32-bit status+sample words, not the final three-byte SD format.
 * This class owns no DMA buffer or ready/free queue. main.cpp supplies each raw
 * block and must not return it to freeBlocks until feed() has finished reading.
 */
class frameDecoder
{
public:
    using outputCallback = bool (*)(const uint32_t*, size_t, void*);
    void setOutput(outputCallback callback, void* context);
    bool outputFailed() const;
    void reset(uint32_t clocksPerFrame);
    void feed(const uint8_t* data, size_t length);
    const captureStats& stats() const;
    uint32_t pendingBits() const;
private:
    void flushOutput();
    outputCallback output = nullptr;
    void* outputContext = nullptr;
    uint32_t outputWords[512] = {};
    size_t outputCount = 0;
    bool outputBroken = false;
    void beginFrame(uint64_t position);
    void appendBits(uint32_t value, unsigned count);
    void clockBit(unsigned pair);
    void finishWord();
    captureStats counts = {};
    uint64_t lastRise = 0;
    uint32_t frameClocks = 0;
    uint32_t word = 0;
    uint32_t bits = 0;
    bool previousHigh = true;
    bool synchronized = false;
};
