// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// Format complete PPS intervals and request file rotation at pair boundaries.
#pragma once
#include "TimedFileFormat.h"
#include "TimedRecordingConfig.h"
#include "TimedFileSink.h"

/**
 * Single-task formatter; no hardware or filesystem dependencies. Call start
 * with the first retained interval's UTC origin, beginBlock with completed
 * timing metadata, then submit its packed samples in small batches. Only after
 * exactly sCount samples can the next block begin. The caller buffers an open
 * PPS interval until its closing timestamp is known; this formatter does not
 * invent timing or let the disk writer see an unfinished tBlock.
 *
 * At pairsPerFile, append the zero end marker, request close and rotate slots.
 * File headers use that file's first PPS UTC; nSec restarts at zero. Stop-on-full
 * never opens a reused slot. A sink failure latches failure and prevents further
 * requests. Storage completion is checked separately by the disk adapter.
 */
class TimedRecordingStream
{
public:
    explicit TimedRecordingStream(TimedFileSink& timedFileSink);
    bool start(uint64_t utcFirst, const TimedRecordingConfig& config);
    bool beginBlock(const TimeBlock& timeBlock);
    bool submitSamples(const uint8_t* packed, uint32_t count);
    bool finish();
    bool getFailed() const;
    bool getFull() const;
private:
    bool closeFile();
    TimedFileSink& _timedFileSink;
    TimedRecordingConfig _config;
    uint64_t _utcFirst = 0, _intervals = 0, _tcPpsLast = 0, _tcFsLast = 0;
    uint32_t _files = 0, _pairs = 0, _remaining = 0;
    bool _active = false, _opened = false, _failed = false, _full = false;
};
