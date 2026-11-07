// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// Hold packed samples until two observed PPS boundaries complete an interval.
#pragma once
#include "TimedBoundary.h"
#include "TimedRecordingStream.h"
#include <atomic>

/**
 * Three roles: the decoder submits packed samples, the timing task adds actual
 * boundaries, and the formatting task calls pump. Each role has one owner.
 * Atomic positions publish completed copies and prevent reuse until consumed.
 * No disk I/O occurs here. The formatting task sends completed intervals to
 * TimedRecordingStream, whose sink owns a separate write buffer.
 * Here, publishing means copying bytes first, then updating an atomic index
 * with release ordering. The reader acquires that index before using the copy.
 * This is software memory synchronization, not an atomic snapshot of hardware
 * pulse-counter and timestamp registers.
 *
 * Sample capacity must be a power of two. On the P4, 2097152 samples occupy
 * 6 MiB and provide about two seconds at 1.024 MSPS. The first interval waits
 * roughly one second for its closing boundary. Overflow fails instead of
 * overwriting samples. Memory belongs to the caller and outlives this object.
 */
class TimedIntervalBuffer
{
public:
    bool start(uint8_t* memory, uint32_t capacity, TimedRecordingStream& stream,
        uint32_t pairsPerFile);
    bool submit(const uint8_t* packed, uint32_t count);
    bool addBoundary(const TimedBoundary& boundary);
    bool pump();
    bool finish();
    bool getFailed() const;
    uint64_t getIntervals() const;
    uint64_t getSamples() const;
    uint32_t getPeak() const;
private:
    void release(uint32_t count);
    uint8_t* _memory = nullptr;
    uint32_t _capacity = 0, _pairsPerFile = 0;
    TimedRecordingStream* _stream = nullptr;
    std::atomic<uint32_t> _head{0}, _tail{0}, _peak{0};
    static constexpr uint32_t _boundaryCapacity = 8;
    TimedBoundary _boundaries[_boundaryCapacity];
    std::atomic<uint32_t> _boundaryHead{0}, _boundaryTail{0};
    std::atomic<bool> _failed{false};
    // Only the formatting task modifies these; read statistics after finish.
    uint64_t _sampleRead = 0, _intervals = 0, _samples = 0;
    uint32_t _remaining = 0;
};
