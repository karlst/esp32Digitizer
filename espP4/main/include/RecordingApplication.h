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

/** @file RecordingApplication.h
 * @brief P4 receive/decode lifecycle, independent of test startup and storage policy.
 */
#pragma once
#include <atomic>
#include "driver/parlio_rx.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "FrameDecoder.h"
#include "CaptureBlock.h"
#include "CaptureDestination.h"
#include "RecordingObserver.h"
#include "BenchLine.h"
/**
 * @brief Own receiver memory, queues and the decoder task for one permanent application.
 * Startup supplies a destination and optional observer, then calls initialize
 * once and poll repeatedly on Core 0. Core 1 decodes private blocks; the ISR
 * copies DMA chunks on Core 0. Neither task borrows hardware-reused DMA memory.
 * All supplied services and this instance must live until reset. Copying is
 * prohibited because task and driver callbacks retain this object's address.
 */
class RecordingApplication
{
public:
    RecordingApplication(CaptureDestination& captureDestination, uint32_t timeoutMs,
                         RecordingObserver* recordingObserver = nullptr);
    RecordingApplication(const RecordingApplication&) = delete;
    RecordingApplication& operator=(const RecordingApplication&) = delete;
    void initialize();
    void poll();
    bool selectDestination(CaptureDestination& captureDestination, uint32_t timeoutMs);
    bool lockMode();
    bool idle() const;
    bool modeSelected() const;
private:
    static bool receiveEntry(parlio_rx_unit_handle_t receiver, const parlio_rx_event_data_t* event, void* context);
    static void decodeTask(void* context);
    static bool recordWords(const uint32_t* words, size_t count, void* context);
    bool receiveBlock(const parlio_rx_event_data_t* event);
    void decode();
    void reply(const char* format, ...);
    void drainDecoder();
    void releaseReceiver();
    esp_err_t startCapture(unsigned stage);
    void stopCapture(const char* kind);
    void handleLine(const char* line);
    CaptureDestination* _captureDestination;
    RecordingObserver* _recordingObserver;
    uint32_t _timeoutMs;
    BenchLine _benchLine;
    static constexpr uart_port_t _featherPort = UART_NUM_1;

    // These are tested starting allocations, not proven minimum/optimal sizes.
    // 32 KiB holds 4 ms of raw stage-6 input. It buys time to COPY completed chunks
    // before DMA reuses them. A larger decoder queue cannot rescue a missed copy.
    // IDF 5.5.6 with our aligned buffer and 64-byte cache alignment divides this into
    // seven 4032-byte chunks, then 2240 and 2304 bytes: nine chunks, NOT eight 4 KiB
    // chunks. See parlio_rx_mount_transaction_buffer(). This is driver-specific.
    // Four DCLKs per byte gives 16128, 8960, 9216 clocks respectively, or 492.19,
    // 273.44, 281.25 us at stage 6. Nine receive callbacks per 4 ms average 2250/s;
    // callback service can be delayed after the hardware completion event.
    static constexpr size_t _dmaBytes = 32768;

    // Private blocks must hold any delivered chunk. Sixteen 4 KiB data arrays use
    // 64 KiB of internal RAM plus metadata/queue overhead. Their nominal capacity
    // is 8 ms of raw input; partly filled blocks and blocks being copied/decoded
    // reduce the waiting capacity. This cushions brief scheduling delays, not a
    // decoder that is persistently slower than input. The early decoder exhausted
    // this pool; optimizing decoding allowed passing without increasing these sizes.
    static constexpr size_t _blockBytes = 4096;
    static constexpr unsigned _blockCount = 16;
    static_assert(std::atomic<uint32_t>::is_always_lock_free,
                  "ISR counters must not invoke a locking implementation");



    CaptureBlock* _captureBlocks = nullptr;

    // Both FreeRTOS queues hold CaptureBlock* addresses into the SAME blocks array.
    // freeBlocks means Core 0 may overwrite the block; readyBlocks means Core 1
    // must read it first. The queues are not another 64 KiB copy of the data.
    QueueHandle_t _freeBlocks = nullptr;
    QueueHandle_t _readyBlocks = nullptr;
    // UART is the serial link between boards. Its event queue reports receive
    // errors separately from command bytes. drained is a semaphore, a completion
    // signal Core 1 gives when it has finished all previously queued raw blocks.
    QueueHandle_t _uartEvents = nullptr;
    SemaphoreHandle_t _drained = nullptr;
    FrameDecoder _frameDecoder;
    TaskHandle_t _decoderTaskHandle = nullptr;
    // Mode is selected on Core 0 before any capture/SD commands after boot and is
    // then fixed until reset. Core 1 reads it only after receiving a queued block.
    bool _modeLocked = false;
    int64_t _stopRequestedAt = 0;
    parlio_rx_unit_handle_t _receiver = nullptr;
    parlio_rx_delimiter_handle_t _delimiter = nullptr;
    uint8_t* _dmaBuffer = nullptr;
    // enabled tracks whether the hardware receiver must be disabled during cleanup.
    // active means a capture request was accepted and has not yet been stopped;
    // it does not mean the ADC has actually supplied any samples.
    bool _enabled = false;
    bool _active = false;
    unsigned _activeStage = 0;
    unsigned long _activeRun = 0;
    // armedAt starts the capture timeout when setup succeeds, before Feather starts
    // the ADC. Atomic counters can be safely updated/read across the interrupt and
    // control code without a separate lock. callbackErrors also includes UART errors
    // detected during capture, despite its narrower historical name.
    int64_t _armedAt = 0;
    std::atomic<uint32_t> _droppedBlocks{0};
    std::atomic<uint32_t> _callbackErrors{0};

    // The ISR alone writes these measurements. CPU0 reads them only after disabling
    // the receiver on CPU0, where its interrupt was installed, and draining CPU1.
    // More than 14 GB passes through DMA in 30 minutes. ISR owns this 64-bit
    // total exclusively; CPU0 reads it only after disabling capture, never live.
    uint64_t _rawBytes = 0;
    uint32_t _firstBlockBytes = 0;
    uint32_t _callbacks = 0;
    int64_t _firstBlockAt = 0;
    int64_t _lastBlockAt = 0;
    uint32_t _queueHighWater = 0;
    int64_t _maxCallbackGapUs = 0;
    int64_t _maxCopyUs = 0;

};
