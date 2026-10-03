/**
 * @file chokeEvents.cpp
 * @brief add() copies numbers; drain() alone formats and writes USB diagnostics.
 * Labels must be string literals; fields names the four numbers in print order.
 */
#include "chokeEvents.h"
#if S3_CHOKE_TEST
#include <esp_timer.h>
#include <cstdio>
portMUX_TYPE chokeEvents::lock = portMUX_INITIALIZER_UNLOCKED;
chokeEvents::event chokeEvents::queue[64] = {};
uint32_t chokeEvents::head = 0, chokeEvents::count = 0;
uint32_t chokeEvents::run = 0, chokeEvents::dropped = 0;
uint64_t chokeEvents::epochUs = 0;
/**
 * @brief Acquisition starts the diagnostic clock before requesting file opening.
 * Keep old queued results: a new Start must not erase an unprinted final result.
 */
void chokeEvents::beginRun()
{
    portENTER_CRITICAL(&lock);
    epochUs = static_cast<uint64_t>(esp_timer_get_time());
    ++run;
    portEXIT_CRITICAL(&lock);
    add("start-request", "unused");
}
/**
 * @brief Enqueue measurements without formatting, allocation, disk I/O or USB waits.
 * Before the first Start, ignore startup scans. important=false reserves slots
 * for rate/Stop events. If even the reserve fills, increment the dropped count.
 */
void chokeEvents::add(const char* name, const char* fields, uint64_t a,
    uint64_t b, uint64_t c, uint64_t d, bool important)
{
    portENTER_CRITICAL(&lock);
    if (run != 0)
    {
        if (count < (important ? 64U : 48U))
        {
            queue[(head + count) % 64] = {name, fields,
                (static_cast<uint64_t>(esp_timer_get_time()) - epochUs) / 1000,
                a, b, c, d, run};
            ++count;
        }
        else
        {
            ++dropped;
        }
    }
    portEXIT_CRITICAL(&lock);
}
/**
 * @brief Try one event per communications pass, retaining it if USB has no room.
 * Only featherLink calls this. Lock only the memory copy; format and transmit
 * outside it. dropped is cumulative since boot, not the current run's total.
 */
void chokeEvents::drain()
{
    static event pending = {};
    static bool havePending = false;
    uint32_t lost = 0;
    portENTER_CRITICAL(&lock);
    if (!havePending && count != 0)
    {
        pending = queue[head];
        head = (head + 1) % 64;
        --count;
        havePending = true;
    }
    lost = dropped;
    portEXIT_CRITICAL(&lock);
    if (havePending)
    {
        char line[320];
        const int length = snprintf(line, sizeof(line),
            "EVENT run=%lu t=%llums %s [%s]=%llu,%llu,%llu,%llu dropped=%lu\n",
            static_cast<unsigned long>(pending.run), static_cast<unsigned long long>(pending.ms),
            pending.name, pending.fields, static_cast<unsigned long long>(pending.a),
            static_cast<unsigned long long>(pending.b), static_cast<unsigned long long>(pending.c),
            static_cast<unsigned long long>(pending.d), static_cast<unsigned long>(lost));
        if (length > 0 && length < static_cast<int>(sizeof(line)) && Serial.availableForWrite() >= length)
        {
            havePending = Serial.write(reinterpret_cast<const uint8_t*>(line), length) != static_cast<size_t>(length);
        }
    }
}
#endif
