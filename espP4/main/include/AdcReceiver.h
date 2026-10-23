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
 * @file AdcReceiver.h
 * Raw receive hardware and ownership of the blocks passed to Core 1.
 */
#pragma once
#include <atomic>
#include "driver/parlio_rx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "CaptureBlock.h"
#include "ReceiverStats.h"

/**
 * Copy DMA chunks into private blocks, then queue their addresses.
 * DMA (Direct Memory Access) hardware fills a cyclic 32 KiB buffer. The Core 0
 * ISR (Interrupt Service Routine) copies each completed chunk before hardware
 * reuses it. Sixteen 4 KiB private blocks cushion decoder scheduling delays.
 * A queue contains addresses, not another copy of the data. SampleDecoder on
 * Core 1 takes a block and releases it only after reading all its bytes.
 * Initialize/start/stop run on Core 0. Keep this object alive until reset.
 */
class AdcReceiver
{
public:
    /* **
     * AdcReceiver - Construction allocates nothing and starts no hardware.
     */
    AdcReceiver() = default;
    AdcReceiver(const AdcReceiver&) = delete;
    AdcReceiver& operator=(const AdcReceiver&) = delete;
    void initialize();
    esp_err_t start(uint32_t clockHz);
    void stop();
    bool getFailed() const;
    ReceiverStats getReceiverStats() const;
    CaptureBlock* take();
    void release(CaptureBlock* captureBlock);
    void markDrain();

    /* **
     * getBlockCount - Number of private data blocks, excluding the drain marker
     * slot.
     */
    static constexpr unsigned getBlockCount()
    {
        return _blockCount;
    }

private:
    static bool receiveEntry(parlio_rx_unit_handle_t receiver,
        const parlio_rx_event_data_t* event, void* context);
    bool receiveBlock(const parlio_rx_event_data_t* event);
    // IDF 5.5.6 splits this aligned buffer into nine chunks, not eight 4 KiB
    // chunks. Every delivered chunk must fit one private block.
    static constexpr size_t _dmaBytes = 32768;
    static constexpr size_t _blockBytes = 4096;
    static constexpr unsigned _blockCount = 16;
    static_assert(std::atomic<uint32_t>::is_always_lock_free,
        "ISR counters must not use locks");
    CaptureBlock* _captureBlocks = nullptr;
    QueueHandle_t _freeBlocks = nullptr;
    QueueHandle_t _readyBlocks = nullptr;
    parlio_rx_unit_handle_t _receiver = nullptr;
    parlio_rx_delimiter_handle_t _delimiter = nullptr;
    uint8_t* _dmaBuffer = nullptr;
    bool _enabled = false;
    std::atomic<uint32_t> _droppedBlocks{0};
    std::atomic<uint32_t> _callbackErrors{0};
    // ISR alone writes these; Core 0 reads them only after stop disables ISR.
    uint64_t _rawBytes = 0;
    uint32_t _firstBlockBytes = 0, _callbacks = 0, _queueHighWater = 0;
    int64_t _firstBlockAt = 0, _lastBlockAt = 0;
    int64_t _maxCallbackGapUs = 0, _maxCopyUs = 0;
};
