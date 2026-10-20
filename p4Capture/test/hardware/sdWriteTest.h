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

/** @file sdWriteTest.h
 * @brief CPU1 synthetic data source for measuring the existing SD writing path.
 */
#pragma once
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdRecorder.h"

/** @brief One finite, explicitly requested test; no ADC or DMA control calls.
 * CPU0 starts only after preparation. CPU1 submits paced batches through the
 * same ring and checksum operation as the decoder. The disk task remains CPU0.
 */
class sdWriteTest
{
public:
    bool initialize(sdRecorder& recorder);
    bool start(uint32_t wordsPerSecond);
    void stop();
private:
    static void taskEntry(void* context);
    void work();
    sdRecorder* destination = nullptr;
    TaskHandle_t task = nullptr;
    std::atomic<bool> busy{false};
    std::atomic<bool> stopRequested{false};
    uint32_t rate = 0;

    // The global test object keeps this 2 KiB batch in internal static RAM,
    // outside the task's call stack. CPU1 alone reads/writes it; submitWords
    // copies its bytes before returning, so the next batch may reuse it.
    uint32_t batch[512] = {};
};
