/*
 * Copyright (C) 2026 Karl Stock
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of ESP32 Digitizer.
 * You may redistribute and/or modify it under the GNU General Public License
 * as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 *
 * Distributed WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License in the repository LICENSE file or <https://www.gnu.org/licenses/>.
 */

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
