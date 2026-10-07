/**
 * @file esp_timer.h
 * @brief Use the same simulated microsecond clock for ESP-IDF timer calls.
 */
#pragma once
#include "Arduino.h"

/**
 * @brief Allow register tests to advance time during the real reader's waits.
 */
inline int64_t esp_timer_get_time() { return micros(); }
