/**
 * @file Arduino.h
 * @brief Minimal serial/delay compatibility for the shared read-only diagnostic.
 * This header is used ONLY by the isolated ESP-IDF project. It supplies no
 * Arduino hardware implementation: SD calls still go directly to ESP-IDF.
 * Keeping the diagnostic source identical reduces differences between tests.
 */
#pragma once
#include <cstdint>
#include <cstddef>
#include "diagnosticSerial.h"
extern diagnosticSerial Serial;
void delay(unsigned long milliseconds);
