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
 * @file FrameDecoder.h
 * @brief Reconstruct ADC samples from saved data-pin and sample-start-pin values.
 * Read feed() first, then beginFrame(), appendBits() and finishWord(). No hardware or RTOS
 * dependency: the same decoder is exercised by the desktop test program.
 */
#pragma once
#include "CaptureStats.h"
#include <cstddef>
#include <cstdint>



/**
 * @brief Core 1's decoder keeps unfinished samples between raw input blocks.
 * A sample is one ADC conversion result. In our selected channel arrangement,
 * FSYNC (frame synchronization) rises once per sample; DCLK provides one clock
 * per serial bit. A rising edge means low-to-high, not every captured high level.
 * FSYNC falls halfway through its period; data continue while it is low. Each packed pair
 * contains DOUT0 in bit 0 and FSYNC in bit 1, earliest pair in byte bits 1:0.
 * Only the first 32 DCLKs of a frame form our 8-bit header + 24-bit sample.
 * Idle clocks in the rest of a frame are deliberately ignored.
 * An optional output callback receives copies of complete raw words in batches
 * of at most 512. A rejected batch sets a persistent failure flag but does not hide diagnostic
 * counts. reset clears that latch while preserving the selected callback.
 * The output is 32-bit status+sample words, not the final three-byte SD format.
 * This class owns no DMA buffer or ready/free queue. SampleDecoder supplies each raw
 * block and must not return it to freeBlocks until feed() has finished reading.
 */
class FrameDecoder
{
public:
    using outputCallback = bool (*)(const uint32_t*, size_t, void*);
    void setOutput(outputCallback callback, void* context);
    bool getOutputFailed() const;
    void reset(uint32_t clocksPerFrame);
    void feed(const uint8_t* data, size_t length);
    const CaptureStats& getCaptureStats() const;
    uint32_t getPendingBits() const;
private:
    void flushOutput();
    outputCallback _output = nullptr;
    void* _outputContext = nullptr;
    uint32_t _outputWords[512] = {};
    size_t _outputCount = 0;
    bool _outputBroken = false;
    void beginFrame(uint64_t position);
    void appendBits(uint32_t value, unsigned count);
    void clockBit(unsigned pair);
    void finishWord();
    CaptureStats _captureStats = {};
    uint64_t _lastRise = 0;
    uint32_t _frameClocks = 0;
    uint32_t _word = 0;
    uint32_t _bits = 0;
    bool _previousHigh = true;
    bool _synchronized = false;
};
