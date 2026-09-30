/**
 * @file bufferedWriter.h
 * @brief Reusable byte recording with an owned ring buffer and measured disk delays.
 */
#pragma once
#include "byteSink.h"
#include <atomic>
static_assert(ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_BOOL_LOCK_FREE == 2,
    "The producer requires lock-free 32-bit and boolean atomics.");
#ifdef ESP_PLATFORM
#include <esp_attr.h>
#define BUFFERED_IRAM IRAM_ATTR
#else
#define BUFFERED_IRAM
#endif

/**
 * @brief Decouple one fast producer from one slower storage task.
 *
 * Producer: submit() copies bytes and returns immediately; it may run in an ISR
 * (interrupt service routine). Consumer: pump() writes queued bytes to byteSink.
 * The consumer releases buffer space ONLY after write() returns successfully.
 * Thus the producer cannot overwrite memory still being used by the disk driver.
 *
 * Only these two contexts may use the ring. Atomic 32-bit positions publish
 * completed copies across CPU cores; no mutex, allocation, or disk call occurs
 * in submit(). start()/finish() require the producer to be stopped. Other tasks
 * must obtain statistics through a snapshot published by the consumer's owner.
 * The buffer is part of this object: on ESP32 put the object in internal RAM,
 * never external PSRAM, because interrupts must access it while flash is busy.
 */
class bufferedWriter
{
public:
    static constexpr uint32_t capacity = 65536;
    /** @brief Consumer-owned totals; bytes are payload, independent of record format. */
    struct statistics
    {
        uint64_t bytes = 0;
        uint64_t elapsedUs = 0;
        uint64_t maxAtUs = 0;
        uint64_t rejectedBytes = 0;
        uint32_t unsavedBytes = 0; // Retained bytes not accepted after a partial write.
        uint32_t used = 0, peak = 0, readPosition = 0, writePosition = 0;
        uint32_t latestUs = 0, maxUs = 0, maxKind = 0;
        uint32_t overflows = 0, errors = 0;
    };
    void start(byteSink& destination, uint64_t (*clockUs)());
    bool BUFFERED_IRAM submit(const uint8_t* data, uint32_t length);
    bool pump(bool drain = false);
    bool finish();
    void cancel();
    bool failed() const;
    statistics snapshot();
    void measure(uint64_t startedUs, uint32_t kind, bool success);
private:
    alignas(4) uint8_t buffer[capacity] = {};
    // Monotonic unsigned counters wrap naturally. Their difference is occupied
    // bytes (at most capacity); masking gives a physical position in the ring.
    std::atomic<uint32_t> head{0}, tail{0}, peak{0}, rejected{0};
    std::atomic<bool> broken{false}, overflowed{false};
    byteSink* sink = nullptr;
    uint64_t (*clockUs)() = nullptr;
    uint64_t startedUs = 0;
    bool active = false;
    uint32_t partialBytes = 0;
    statistics totals;
};
