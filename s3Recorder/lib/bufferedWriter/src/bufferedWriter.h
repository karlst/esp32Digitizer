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
    // Bytes, not samples: 65536 / 4 = 16384 current-format samples, roughly
    // 0.55 seconds of storage delay at 30000 samples/second. Must remain a power of two.
    static constexpr uint32_t capacity = 65536;
    /**
     * @brief Consumer-owned totals; bytes are payload, independent of record format.
     */
    struct statistics
    {
        // Payload bytes accepted by write(), excluding file headers the adapter writes.
        uint64_t bytes = 0;
        // Microseconds since writer.start(); includes file preparation and final saving.
        uint64_t elapsedUs = 0;
        uint64_t maxAtUs = 0;
        // Bytes explicitly offered but refused by submit(); not unseen ADC events.
        // The producer accumulator is 32-bit; this widened snapshot does not extend it.
        uint64_t rejectedBytes = 0;
        uint32_t unsavedBytes = 0; // Retained bytes not accepted after a partial write.
        uint32_t used = 0, peak = 0, readPosition = 0, writePosition = 0;
        uint32_t latestUs = 0, maxUs = 0, maxKind = 0;
        // overflows is a latched 0/1 incident flag per recording. errors counts failed
        // measured storage calls; neither counter estimates every possible physical loss.
        uint32_t overflows = 0, errors = 0;
    };
    void start(byteSink& destination, uint64_t (*clockUs)());
    bool BUFFERED_IRAM submit(const uint8_t* data, uint32_t length);
    bool pump(bool drain = false);
    bool finish();
    void cancel();
    bool failed() const;
    statistics snapshot();
    uint32_t queuedBytes() const;
    void measure(uint64_t startedUs, uint32_t kind, bool success);
private:
    // The actual ring memory. alignas(4) puts its start on a four-byte boundary.
    // Callers cannot borrow writable pointers into it and bypass the queue rules.
    alignas(4) uint8_t buffer[capacity] = {};
    // Monotonic unsigned counters wrap naturally. Their difference is occupied
    // bytes (at most capacity); masking gives a physical position in the ring.
    // Producer writes head/peak/rejected; consumer writes tail. Atomic means the
    // other CPU cannot observe half of an update. This does NOT make an entire
    // operation atomic; correctness also depends on exactly one producer/consumer.
    std::atomic<uint32_t> head{0}, tail{0}, peak{0}, rejected{0};
    // Both sides can report failure through broken. overflowed specifically
    // remembers running out of ring space, even if a later flush also fails.
    std::atomic<bool> broken{false}, overflowed{false};
    // Borrowed destination; all fields below are used by the disk task only.
    byteSink* sink = nullptr;
    uint64_t (*clockUs)() = nullptr;
    uint64_t startedUs = 0;
    bool active = false;
    // Accepted prefix of a failed block. Used for accounting, never automatic retry.
    uint32_t partialBytes = 0;
    statistics totals;
};
