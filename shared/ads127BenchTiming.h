/**
 * @file ads127BenchTiming.h
 * @brief Shared finite-run timing for the ADC controller and P4 receiver.
 * ADS127_P4_ENDURANCE selects 30 minutes; ADS127_P4_LONG_RUN retains 60 seconds.
 * The historical Feather/S3 build retains its five-second run/eight-second
 * receiver deadline. Both P4 test boards must be rebuilt for the long run.
 */
#pragma once
#include <cstdint>

namespace ads127BenchTiming
{
#if defined(ADS127_P4_ENDURANCE) && ADS127_P4_ENDURANCE
static constexpr uint32_t runMs = 1800000;
static constexpr uint32_t receiverTimeoutMs = 1805000;
#elif defined(ADS127_P4_LONG_RUN) && ADS127_P4_LONG_RUN
static constexpr uint32_t runMs = 60000;
static constexpr uint32_t receiverTimeoutMs = 65000;
#else
static constexpr uint32_t runMs = 5000;
static constexpr uint32_t receiverTimeoutMs = 8000;
#endif

// Feather waits up to two seconds for READY before starting. The receiver's
// deadline starts earlier, at ARM, so leave additional time for stop/response.
static_assert(receiverTimeoutMs >= runMs + 3000, "Receiver deadline needs handshake margin");

// P4 raw-byte and file-byte totals are 64-bit. Valid sample/header counts stay
// 32-bit to preserve the per-sample decoder cost. Bound these at the receiver
// deadline with 10% clock margin; future longer runs require another review.
static_assert(uint64_t{1126400} * receiverTimeoutMs / 1000 < UINT32_MAX,
              "Extend sample counters before increasing duration");
static_assert(receiverTimeoutMs < 0x80000000U, "Keep millisecond wrap subtraction bounded");
}
