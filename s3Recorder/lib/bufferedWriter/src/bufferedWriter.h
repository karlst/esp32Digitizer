/**
 * @file bufferedWriter.h
 * @brief Reusable byte recording with an owned ring buffer and measured disk delays.
 */
#pragma once
#include "byteSink.h"
#include <atomic>
#ifndef BUFFERED_WRITER_CAPACITY
#define BUFFERED_WRITER_CAPACITY 65536
#endif
#ifndef BUFFERED_WRITER_BLOCK_BYTES
#define BUFFERED_WRITER_BLOCK_BYTES 4096
#endif
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
 * Default builds keep the ring inside this object in internal RAM for ISR use.
 * External-buffer builds instead own memory obtained through initializeMemory().
 * Their submit() must run in a TASK, never a cache-disabled interrupt: PSRAM can
 * be inaccessible when flash operations disable the cache. Keep the control
 * object/atomic counters in internal RAM in either case.
 */
class bufferedWriter
{
public:
    // Configuration is in bytes; power-of-two capacity permits cheap wraparound.
    // All translation units must use the same build flags (including consumers).
    static constexpr uint32_t capacity = BUFFERED_WRITER_CAPACITY;
    static constexpr uint32_t blockBytes = BUFFERED_WRITER_BLOCK_BYTES;
    static_assert(capacity && !(capacity & (capacity - 1)) && capacity < 0x80000000u,
        "Ring capacity must be a power of two below 2 GiB");
    static_assert(blockBytes && capacity % blockBytes == 0, "Blocks must divide the ring");
#if BUFFERED_WRITER_EXTERNAL

    /** @brief Create an unallocated writer; initializeMemory must precede start. */
    bufferedWriter() = default;
    ~bufferedWriter();
    bool initializeMemory(uint8_t* (*allocate)(uint32_t), void (*release)(uint8_t*));
    bufferedWriter(const bufferedWriter&) = delete;
    bufferedWriter& operator=(const bufferedWriter&) = delete;
#endif

    /**
     * @brief Consumer-owned totals; bytes are payload, independent of record format.
     */
    struct statistics
    {
        // Payload bytes accepted by write(), excluding file headers the adapter writes.
        uint64_t bytes = 0;

        // Microseconds since writer.start(); excludes earlier file preparation, includes final saving.
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
#if BUFFERED_WRITER_EXTERNAL

    // Owned allocation, not a borrowed mutable array. The platform supplies the
    // allocation/free functions; this reusable library has no PSRAM dependency.
    uint8_t* buffer = nullptr;
    void (*releaseMemory)(uint8_t*) = nullptr;
#else
    alignas(4) uint8_t buffer[capacity] = {};
#endif

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
