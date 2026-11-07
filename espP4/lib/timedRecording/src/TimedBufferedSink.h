// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// Ordered file boundaries plus the existing reusable byte ring/chunk writer.
#pragma once
#include "TimedFileSink.h"
#include "TimedSlotStorage.h"
#include "BufferedWriter.h"
#include <atomic>

/**
 * One task queues file boundaries and copies bytes; one disk task pumps them.
 * Both may run on the same core, but neither may change roles during a session.
 * The byte ring is exactly BufferedWriter, including its configured 128-KiB
 * chunk size. A small parallel queue identifies where each chunk must split
 * across physical files. The queue stores offsets, never borrowed data pointers.
 *
 * An accepted endFile is a queued boundary, not completed SD I/O. A descriptor
 * remains occupied until its actual file close completes. Full queues, short
 * writes and failed closes latch failure. No partial write is retried.
 */
class TimedBufferedSink : public TimedFileSink, private ByteSink
{
public:
    explicit TimedBufferedSink(TimedSlotStorage& timedSlotStorage);
    bool initializeMemory(uint8_t* (*allocate)(uint32_t), void (*release)(uint8_t*));
    bool start(uint32_t slotCount, uint64_t (*clockUs)());
    bool beginFile(uint32_t iSlot) override;
    bool submit(const uint8_t* data, size_t bytes) override;
    bool endFile() override;
    bool pump(bool drain = false);
    bool finish();
    bool getFailed() const;
    BufferedWriter::Statistics getStatistics();
private:
    /**
     * Producer writes start/slot before publishing queue head. It writes end
     * before releasing ended. Consumer reads those fields only after acquiring
     * their corresponding flag. This makes 64-bit offsets safe on a 32-bit CPU.
     */
    struct Boundary
    {
        uint64_t _start = 0, _end = 0;
        uint32_t _iSlot = 0;
        std::atomic<bool> _ended{false};
    };
    size_t write(const uint8_t* data, size_t bytes) override;
    bool flush() override;
    bool closeCompleted();
    TimedSlotStorage& _timedSlotStorage;
    BufferedWriter _bufferedWriter;
    static constexpr uint32_t _boundaryCount = 32;
    Boundary _boundaries[_boundaryCount];
    std::atomic<uint32_t> _head{0}, _tail{0};
    std::atomic<bool> _failed{false};
    // Fixed before either task starts; reset only after disk finish returns.
    uint32_t _slotCount = 0;
    bool _active = false, _memoryReady = false;
    uint64_t (*_clockUs)() = nullptr;
    // Submission-task state. Disk finish reads producerOpen only after join.
    uint64_t _submitted = 0;
    bool _producerOpen = false;
    // Disk-task state. Other tasks read statistics only after finish.
    uint64_t _written = 0;
    bool _diskOpen = false;
};
