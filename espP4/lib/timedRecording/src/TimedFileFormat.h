// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// Exact on-disk byte layout, independent of compiler padding and board hardware.
#pragma once
#include "TimedFileHeader.h"
#include "TimeBlock.h"
#include <cstddef>

/**
 * Encode version one into caller-owned buffers. Integers are little-endian;
 * samples retain the existing signed 24-bit, least-significant-byte-first order.
 * Functions never allocate, access storage or interpret a GPS message. Invalid
 * input returns false before modifying the caller's output bytes.
 */
class TimedFileFormat
{
public:
    static constexpr size_t _headerBytes = 256;
    static constexpr size_t _tBlockBytes = 48;
    static constexpr uint16_t _version = 1;
    static bool encodeHeader(const TimedFileHeader& header, uint8_t* data, size_t capacity);
    static bool encodeBlock(const TimeBlock& timeBlock, uint8_t* data, size_t capacity);
    static bool validBlock(const TimeBlock& timeBlock);
    static void encodeEnd(uint8_t* data);
};
