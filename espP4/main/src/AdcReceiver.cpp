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
 * @file AdcReceiver.cpp
 * Receive packed ADC pin states; no decoding, disk access or commands.
 * Read initialize, start, receiveBlock, then stop. PARLIO (parallel I/O) samples
 * DOUT0 and FSYNC on each rising DCLK edge (low-to-high transition). Four clock
 * positions become one byte; DOUT0 uses bits 0,2,4,6 and FSYNC uses 1,3,5,7.
 * The input clock comes from the ADC. Stopping can leave a partial DMA chunk
 * undelivered; zero software drops does not establish lossless hardware capture.
 */
#include "AdcReceiver.h"
#include <cstring>
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#if !CONFIG_ESP32P4_SELECTS_REV_LESS_V3 || !CONFIG_ESPTOOLPY_FLASHSIZE_16MB
#error "This board requires P4 pre-v3 silicon support and 16 MB flash."
#endif

/* **
 * initialize - Core 0 allocates permanent blocks and pointer queues before any
 * capture. Seventeen ready slots hold sixteen blocks plus a null drain marker.
 * GPIO means general-purpose input/output: pins 0,1,2 are inputs without pull
 * resistors. Failed allocation aborts startup; no interrupt ever allocates
 * memory.
 */
void AdcReceiver::initialize()
{
    gpio_config_t inputs = {};
    inputs.pin_bit_mask = (1ULL << 0) | (1ULL << 1) | (1ULL << 2);
    inputs.mode = GPIO_MODE_INPUT;
    ESP_ERROR_CHECK(gpio_config(&inputs));

    // Only an address is copied into either queue. The data stay in this array.
    _captureBlocks = static_cast<CaptureBlock*>(heap_caps_calloc(
        _blockCount, sizeof(CaptureBlock), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    _freeBlocks = xQueueCreate(_blockCount, sizeof(CaptureBlock*));
    _readyBlocks = xQueueCreate(_blockCount + 1, sizeof(CaptureBlock*));
    ESP_ERROR_CHECK(_captureBlocks && _freeBlocks && _readyBlocks ? ESP_OK : ESP_ERR_NO_MEM);
    for (unsigned iCaptureBlock = 0; iCaptureBlock < _blockCount; ++iCaptureBlock)
    {
        CaptureBlock* captureBlock = &_captureBlocks[iCaptureBlock];
        xQueueSend(_freeBlocks, &captureBlock, 0);
    }
}

/* **
 * receiveEntry - The driver calls here in its interrupt; context identifies the
 * registered receiver.
 */
bool AdcReceiver::receiveEntry(parlio_rx_unit_handle_t receiver,
    const parlio_rx_event_data_t* event, void* context)
{
    (void)receiver;
    return static_cast<AdcReceiver*>(context)->receiveBlock(event);
}

/* **
 * receiveBlock - The receive driver calls this on Core 0 when a raw DMA chunk is
 * ready. It runs inside the driver's ISR (Interrupt Service Routine), not a
 * normal task. event describes the delivered bytes; receiveEntry has already
 * used the driver context to select this receiver instance. event->data points
 * into RAM that hardware will reuse, so the decoder must receive a private copy
 * rather than this pointer. memcpy is the actual data transfer into our
 * queue-owned storage. For example, block 3 stays unavailable to this ISR while
 * CPU1 reads it, even after DMA has wrapped around its own 32 KiB area. An
 * invalid chunk increments callbackErrors; no free block or a failed queue send
 * increments droppedBlocks. Core 0's main loop sees these counters and stops
 * capture; this callback itself does not. Returning true asks the scheduler to
 * wake a newly runnable higher-priority task. No logging, heap allocation,
 * decoding or waiting occurs in the ISR. The driver supplies event->data and
 * recv_bytes for this completed chunk; they are packed pin states, not decoded
 * sample values. FromISR queue calls are the interrupt-safe versions: they never
 * wait for another task to run.
 */
bool AdcReceiver::receiveBlock(const parlio_rx_event_data_t* event)
{
    // These timestamps measure when software handles completed chunks, not
    // individual ADC sample times. rawBytes counts driver-reported bytes,
    // including a chunk that the checks below might reject.
    BaseType_t awakened = pdFALSE;
    const int64_t now = esp_timer_get_time();
    if (_callbacks == 0)
    {
        _firstBlockAt = now;
        _firstBlockBytes = static_cast<uint32_t>(event->recv_bytes);
    }
    else if (now - _lastBlockAt > _maxCallbackGapUs)
    {
        _maxCallbackGapUs = now - _lastBlockAt;
    }
    _lastBlockAt = now;
    ++_callbacks;
    _rawBytes += static_cast<uint32_t>(event->recv_bytes);

    // Reject invalid data or lack of private storage without waiting. Atomic
    // counters let the main loop notice the problem and stop the run later.
    CaptureBlock* captureBlock = nullptr;
    if (event->data == nullptr || event->recv_bytes == 0 || event->recv_bytes > _blockBytes)
    {
        _callbackErrors.fetch_add(1, std::memory_order_relaxed);
    }
    else if (xQueueReceiveFromISR(_freeBlocks, &captureBlock, &awakened) != pdTRUE)
    {
        _droppedBlocks.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        // The free queue gave us exclusive use of this block. Copy the actual
        // received length, not its 4096-byte capacity (chunks can be shorter).
        captureBlock->_length = event->recv_bytes;
        captureBlock->_arrivedAt = now;
        memcpy(captureBlock->_data, event->data, captureBlock->_length);
        const int64_t copyUs = esp_timer_get_time() - now;
        if (copyUs > _maxCopyUs)
        {
            _maxCopyUs = copyUs;
        }

        // &block lets FreeRTOS copy the pointer value into the queue. It does
        // not copy the pointed-to bytes. CPU1 may start reading after this send.
        if (xQueueSendFromISR(_readyBlocks, &captureBlock, &awakened) != pdTRUE)
        {
            _droppedBlocks.fetch_add(1, std::memory_order_relaxed);
            xQueueSendFromISR(_freeBlocks, &captureBlock, &awakened);
        }

        // Count waiting blocks only, excluding a block already held by CPU1.
        const uint32_t queued = uxQueueMessagesWaitingFromISR(_readyBlocks);
        if (queued > _queueHighWater)
        {
            _queueHighWater = queued;
        }
    }
    return awakened == pdTRUE;
}

/* **
 * stop - Core 0 stops receiving and frees this run's hardware receiver and DMA
 * RAM. Used by RecordingApplication::stop() and failed setup. enabled and the
 * non-null handles identify which setup steps succeeded. Disable reception
 * before deleting driver objects or freeing memory, so hardware cannot write
 * into freed RAM. ESP_ERROR_CHECK aborts on a driver cleanup error. This does
 * not stop the ADC itself: Feather controls that. Private raw blocks and the
 * decoder task remain allocated for later runs; SampleDecoder::drain() waits for
 * their current work.
 */
void AdcReceiver::stop()
{
    if (_enabled)
    {
        ESP_ERROR_CHECK(parlio_rx_unit_disable(_receiver));
        _enabled = false;
    }
    if (_receiver != nullptr)
    {
        ESP_ERROR_CHECK(parlio_del_rx_unit(_receiver));
        _receiver = nullptr;
    }
    if (_delimiter != nullptr)
    {
        ESP_ERROR_CHECK(parlio_del_rx_delimiter(_delimiter));
        _delimiter = nullptr;
    }
    heap_caps_free(_dmaBuffer);
    _dmaBuffer = nullptr;
}

/* **
 * start - Core 0 starts hardware for an expected external clock in hertz.
 * Success means ready to receive, not that samples arrived. On failure stop
 * releases partial setup; the application must still drain queued decoding.
 */
esp_err_t AdcReceiver::start(uint32_t clockHz)
{
    // Core 0 starts a run only after previous decoding has drained.
    _droppedBlocks.store(0);
    _callbackErrors.store(0);
    _rawBytes = _firstBlockBytes = _callbacks = _queueHighWater = 0;
    _firstBlockAt = _lastBlockAt = 0;
    _maxCallbackGapUs = _maxCopyUs = 0;

    // Allocate the hardware's raw buffer with 64-byte address alignment for
    // the driver/cache requirements. Capability flags request internal RAM
    // usable by DMA and accessible byte by byte. A null pointer is a setup
    // failure; subsequent driver steps run only while retVal remains ESP_OK.
    _dmaBuffer = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        64, _dmaBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    esp_err_t retVal = _dmaBuffer != nullptr ? ESP_OK : ESP_ERR_NO_MEM;

    // Reserve one receive transaction, repeatedly using the same raw buffer.
    // data_width=2 captures DOUT0 and FSYNC together; the ADC supplies the
    // clock. dma_burst_size=16 requests 16-byte hardware transfer bursts, not
    // 16-byte ADC samples. NC means "not connected": unused pins are unassigned.
    parlio_rx_unit_config_t config = {};
    config.trans_queue_depth = 1;
    config.max_recv_size = _dmaBytes;
    config.dma_burst_size = 16;
    config.data_width = 2;
    config.clk_src = PARLIO_CLK_SRC_EXTERNAL;
    config.ext_clk_freq_hz = clockHz;
    config.exp_clk_freq_hz = config.ext_clk_freq_hz;
    config.clk_in_gpio_num = GPIO_NUM_0;
    config.clk_out_gpio_num = GPIO_NUM_NC;
    config.valid_gpio_num = GPIO_NUM_NC;
    for (auto& pin : config.data_gpio_nums)
    {
        pin = GPIO_NUM_NC;
    }
    config.data_gpio_nums[0] = GPIO_NUM_2;
    config.data_gpio_nums[1] = GPIO_NUM_1;
    if (retVal == ESP_OK)
    {
        retVal = parlio_new_rx_unit(&config, &_receiver);
    }

    // A "software delimiter" is the driver's setting for software-controlled
    // receive boundaries, rather than a separate input pin controlling them.
    // POS samples on the rising clock edge. LSB places the earliest pin pair
    // in the byte's lowest two bits; it does not reverse the ADC word, which
    // arrives most-significant bit first. eof_data_len describes the raw
    // buffer length, not one ADC sample. Chunk boundaries need not coincide
    // with sample boundaries; the decoder carries unfinished words forward.
    parlio_rx_soft_delimiter_config_t delimiterConfig = {};
    delimiterConfig.sample_edge = PARLIO_SAMPLE_EDGE_POS;
    delimiterConfig.bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB;
    delimiterConfig.eof_data_len = _dmaBytes;
    if (retVal == ESP_OK)
    {
        retVal = parlio_new_rx_soft_delimiter(&delimiterConfig, &_delimiter);
    }
    // Register the function the driver calls for each completed raw chunk.
    // Enabling with true clears the driver's previous receive requests.
    // Keep enabled accurate so cleanup also works after a later setup failure.
    parlio_rx_event_callbacks_t events = {};
    events.on_partial_receive = receiveEntry;
    if (retVal == ESP_OK)
    {
        retVal = parlio_rx_unit_register_event_callbacks(_receiver, &events, this);
    }
    if (retVal == ESP_OK)
    {
        retVal = parlio_rx_unit_enable(_receiver, true);
        _enabled = retVal == ESP_OK;
    }
    if (retVal == ESP_OK)
    {
        retVal = parlio_rx_soft_delimiter_start_stop(_receiver, _delimiter, true);
    }
    // partial_rx_en selects continuous reuse of the raw buffer and callbacks
    // for completed pieces. receive() is the call that hands dmaBuffer to
    // the driver; it does not wait for all the incoming data to arrive.
    parlio_receive_config_t receiveConfig = {};
    receiveConfig.delimiter = _delimiter;
    receiveConfig.flags.partial_rx_en = true;
    if (retVal == ESP_OK)
    {
        retVal = parlio_rx_unit_receive(_receiver, _dmaBuffer, _dmaBytes, &receiveConfig);
    }
    if (retVal != ESP_OK)
    {
        stop();
    }
    return retVal;
}

/* **
 * getFailed - Core 0 can read these atomic fault counters during capture.
 */
bool AdcReceiver::getFailed() const
{
    return _droppedBlocks.load() != 0 || _callbackErrors.load() != 0;
}

/* **
 * getReceiverStats - Core 0 copies measurements only after stop has disabled the
 * interrupt.
 */
ReceiverStats AdcReceiver::getReceiverStats() const
{
    ReceiverStats receiverStats;
    receiverStats._rawBytes = _rawBytes;
    receiverStats._firstBlockBytes = _firstBlockBytes;
    receiverStats._callbacks = _callbacks;
    receiverStats._firstBlockAt = _firstBlockAt;
    receiverStats._lastBlockAt = _lastBlockAt;
    receiverStats._queueHighWater = _queueHighWater;
    receiverStats._maxCallbackGapUs = _maxCallbackGapUs;
    receiverStats._maxCopyUs = _maxCopyUs;
    receiverStats._droppedBlocks = _droppedBlocks.load();
    receiverStats._callbackErrors = _callbackErrors.load();
    return receiverStats;
}

/* **
 * take - Core 1 sleeps until a block or null drain marker arrives; caller owns
 * the block.
 */
CaptureBlock* AdcReceiver::take()
{
    CaptureBlock* captureBlock = nullptr;
    xQueueReceive(_readyBlocks, &captureBlock, portMAX_DELAY);
    return captureBlock;
}

/* **
 * release - Core 1 returns a fully read block, permitting the ISR to overwrite
 * it.
 */
void AdcReceiver::release(CaptureBlock* captureBlock)
{
    xQueueSend(_freeBlocks, &captureBlock, portMAX_DELAY);
}

/* **
 * markDrain - After stopping hardware, Core 0 queues a marker behind every
 * received block.
 */
void AdcReceiver::markDrain()
{
    CaptureBlock* captureBlock = nullptr;
    xQueueSend(_readyBlocks, &captureBlock, portMAX_DELAY);
}
