/**
 * @file esp_system.h
 * @brief Replace the hardware random-number function with a repeatable test seed.
 * Production firmware uses esp_random() from the real ESP framework so command
 * sequences differ across boots. This fake only makes desktop assertions predictable.
 */
#pragma once
/**
 * @brief Return a known starting ID so acknowledgement tests are reproducible.
 */
inline uint32_t esp_random() { return 100; }
