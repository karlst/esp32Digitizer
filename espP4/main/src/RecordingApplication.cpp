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
 * @file RecordingApplication.cpp
 * @brief Receive measurements from the ADS127L18 and save them to SD or inspect them in RAM.
 *
 * This is the P4 program. The Feather sets up the ADC (analog-to-digital
 * converter) and tells it when to start and stop measuring. The P4 receives
 * the resulting numbers. It does not control the analog test voltage or
 * configure the ADC, and powering it up does not start a recording.
 *
 * Ordinary startup supplies SD storage. Test startup can also supply RAM inspection:
 * - SD recording: type 'p' in the P4's PC terminal, wait for SD READY, then
 *   start a test from the Feather. Every decoded sample is stored as three
 *   bytes, without averaging. SD SAVED is the later storage result; a DONE
 *   message from the receiver does not mean the file has finished saving.
 * - RAM inspection (test builds only): type 'w' before SD preparation or capture.
 *   The same receiver and decoder run, but samples go to a waveform summary
 *   instead of a file. Skip the first second of decoded samples, then collect
 *   300 ms in 600 groups of 0.5 ms each. Keep each group's minimum, maximum,
 *   and sum; print its average when reporting. At stage 6 that is 512 samples
 *   per group. This is a summary, not a copy of every individual sample.
 *   The card is not accessed, although the SD task and its RAM still exist.
 *   Reset the P4 before changing between these modes.
 *
 * Start reading at initialize(), then poll(). The small startup file calls them.
 * Initialization allocates memory and starts the decoder task; poll repeatedly
 * checks commands and capture errors. Startup initializes storage separately.
 * A task is a software thread scheduled to run on a processor core. Core 0
 * handles commands and SD writing; Core 1 decodes received data. These jobs
 * can progress independently, sharing data through queues and buffers.
 *
 * Follow one recording through these functions:
 * 1. handleLine() receives the Feather's request to prepare the receiver.
 * 2. startCapture() sets up receiving, then READY2 tells the Feather it may
 *    start the ADC. READY2 does not establish that any data have arrived.
 * 3. receiveBlock() copies each delivered raw block out of hardware-owned RAM.
 * 4. decodeTask() turns those bits into numbers. recordWords() sends them to
 *    the chosen SD buffer or RAM summary.
 * 5. stopCapture() stops receiving and waits for queued decoding to finish.
 *    It then reports the RAM summary or asks the SD task to finish the file.
 * Read SdRecorder.cpp here for the actual card writes and
 * test/hardware/RamWaveform.cpp (under espP4) for the summary calculations. The hardware and memory details follow below.
 *
 * A sample is one ADC measurement. The Feather
 * configures and starts/stops the ADC; this P4 receives and records its output.
 * DCLK is the ADC's output bit clock, DOUT0 its data wire, and FSYNC a separate
 * wire whose low-to-high transition marks the start of a new sample frame.
 * An edge is a change from low to high voltage, or high to low. A stage is
 * an entry in ads127BenchRates, the shared table of test sampling rates.
 * At stage 6 (1,024,000 samples/s), DCLK is 32.768 MHz: one cycle every
 * 30.52 ns, about 15.26 ns each high/low half. The ADC changes data after
 * falling edges; PARLIO captures both
 * DOUT0 and FSYNC on rising edges. These are hardware actions, not CPU calls.
 *
 * PARLIO (parallel input/output) captures both pins at once. Four clock instants
 * become one packed byte: DOUT0 occupies bits 0,2,4,6 and FSYNC bits 1,3,5,7;
 * the earliest pair is in bits 1:0. PARLIO's hardware receive FIFO (First In,
 * First Out) preserves arrival order before DMA transfers the stream to RAM.
 * Its exact FIFO capacity/internal packing-write width is not established here;
 * neither should be confused with the software buffer sizes below.
 * GDMA (General Direct Memory Access) is transfer hardware, not memory. The P4
 * PARLIO driver uses GDMA-AXI (verified in IDF's esp32p4 gdma_channel.h and
 * parlio_priv.h), not GDMA-AHB. The driver configures it; neither CPU moves each
 * pair of input bits. DMA transfers use the internal bus timing, not one CPU
 * operation or interrupt per DCLK edge.
 *
 * Data path and ownership:
 * 1. Hardware fills the 32 KiB DMA RAM area cyclically (reusing its beginning).
 * 2. A completed descriptor (address/length instructions for one DMA chunk)
 *    causes a DMA interrupt. Core 0 services the driver's ISR, an Interrupt
 *    Service Routine that briefly interrupts normal task execution. The driver
 *    calls receiveBlock() in that same interrupt context.
 * 3. receiveBlock() copies raw bytes into one of 16 private 4 KiB blocks and
 *    queues its ADDRESS in readyBlocks. The block stays at its RAM address.
 * 4. Core 1's decodeTask takes that address, calls decoder.feed(), and sends
 *    decoded samples to the SD buffer or RAM summary, depending on the mode.
 *    Only then does it return the raw block's address to freeBlocks for reuse.
 * 5. In SD mode, the independent Core 0 sdWriter task drains the 4 MiB
 *    PSRAM recording ring through sdRecorder::write(). An interrupt can pause
 *    this task while its handler runs. A queued
 *    sample is not yet saved; final SD SAVED follows file readback and closing.
 *
 * PSRAM is additional RAM outside the P4 chip. A ring buffer reuses its
 * beginning after reaching its end, but only after earlier data have been
 * consumed. Here it holds decoded bytes waiting for the SD card. It is
 * separate from both the hardware's raw buffer and the private raw blocks.
 *
 * A frame is the group of clock positions belonging to one ADC sample.
 * At stage 6 each frame is 8 status bits + 24 sample bits on DOUT0, with a
 * captured FSYNC bit alongside EVERY bit: 32 clocks x 2 bits = 8 raw bytes.
 * Decoding checks status and packs only the sample into 3 file bytes. Input is
 * 8.192 MB/s; sample payload is 3.072 MB/s (24.576 Mbps). MB is decimal; KiB and
 * MiB mean 1024 and 1048576 bytes. Lower stages can have unused frame clocks.
 *
 * Core 0 also stops reception if the allowed capture time expires. Neither
 * decoder.feed() nor SD file writing runs inside the receive interrupt handler.
 *
 * A free-buffer queue transfers ownership to the ISR; a ready-buffer queue
 * transfers it to the decoder. No queued pointer refers to a DMA buffer that
 * hardware can overwrite. A full software queue is a fault, never permission
 * to discard quietly. However, zero reported drops does not prove lossless
 * hardware reception: the driver supplies no guaranteed counter of raw data
 * overwritten before software copies it. Stopping can also leave an
 * incomplete hardware block undelivered. See README.md for these limitations.
 */
#include "RecordingApplication.h"
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include "driver/gpio.h"
#include "driver/parlio_rx.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "Ads127BenchRate.h"
#include "BenchLine.h"
#include "FrameDecoder.h"
#include "sdkconfig.h"

#if !CONFIG_ESP32P4_SELECTS_REV_LESS_V3 || !CONFIG_ESPTOOLPY_FLASHSIZE_16MB
#error "This bench project requires P4 pre-v3 silicon support and 16 MB flash."
#endif

#if !CONFIG_SPIRAM
#error "SD recording needs PSRAM: build with sdkconfig.sd.defaults as documented."
#endif

/** @brief Borrow startup-selected services and retain the capture deadline in milliseconds. */
RecordingApplication::RecordingApplication(CaptureDestination& captureDestination, uint32_t timeoutMs,
    RecordingObserver* recordingObserver)
    : _captureDestination(&captureDestination), _recordingObserver(recordingObserver), _timeoutMs(timeoutMs) {}
/** @brief Select a destination only before any mode or recording request since boot. */
bool RecordingApplication::selectDestination(CaptureDestination& captureDestination, uint32_t timeoutMs)
{
    const bool retVal = !_modeLocked && !_active;
    if (retVal) { _captureDestination = &captureDestination; _timeoutMs = timeoutMs; _modeLocked = true; }
    return retVal;
}
/** @brief Prevent later destination changes; return whether no capture is active. */
bool RecordingApplication::lockMode() { _modeLocked = true; return !_active; }
/** @brief Report receiver inactivity, not whether a disk write has completed. */
bool RecordingApplication::idle() const { return !_active; }
/** @brief Report whether a destination or preparation/capture request has locked the mode. */
bool RecordingApplication::modeSelected() const { return _modeLocked; }
/** @brief Driver ISR trampoline; context is the permanent application registered at setup. */
bool RecordingApplication::receiveEntry(parlio_rx_unit_handle_t receiver,
    const parlio_rx_event_data_t* event, void* context)
{ (void)receiver; return static_cast<RecordingApplication*>(context)->receiveBlock(event); }
/** @brief FreeRTOS task trampoline; dispatch to the owning application's endless decoder loop. */
void RecordingApplication::decodeTask(void* context)
{ static_cast<RecordingApplication*>(context)->decode(); }

/**
 * @brief Core 0 formats a control reply for Feather and prints the same text on the PC.
 * Callers supply printf-style format text and values. Replies describe setup,
 * stop requests, or results; READY2 and RAMFULL can be sent during an active
 * capture. This function is never called from the receive interrupt handler.
 * uart_write_bytes() queues text for UART1 transmission on GPIO5; printf()
 * writes the PC console. Neither is confirmation that Feather received it.
 * Formatting failure or a line too large for the 384-byte array sends nothing.
 */
void RecordingApplication::reply(const char* format, ...)
{
    // Build one complete line before sending it; length excludes the final
    // string terminator. Do not send a truncated command that Feather could
    // misinterpret as a different request.
    char message[384];
    va_list arguments;
    va_start(arguments, format);
    const int length = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (length > 0 && static_cast<size_t>(length) < sizeof(message))
    {
        uart_write_bytes(_featherPort, message, length);
        printf("%s", message);
    }
}

/**
 * @brief The receive driver calls this on Core 0 when a raw DMA chunk is ready.
 * It runs inside the driver's ISR (Interrupt Service Routine), not a normal
 * task. event describes the delivered bytes; receiveEntry has already used the
 * driver context to select this application instance.
 * event->data points into RAM that hardware will reuse, so the decoder must
 * receive a private copy rather than this pointer.
 * memcpy is the actual data transfer into our queue-owned storage. For example,
 * block 3 stays unavailable to this ISR while CPU1 reads it, even after DMA has
 * wrapped around its own 32 KiB area. An invalid chunk increments callbackErrors;
 * no free block or a failed queue send increments droppedBlocks. Core 0's main
 * loop sees these counters and stops capture; this callback itself does not.
 * Returning true asks the scheduler to wake a newly runnable higher-priority
 * task. No logging, heap allocation, decoding or waiting occurs in the ISR.
 * The driver supplies event->data and recv_bytes for this completed chunk;
 * they are packed pin states, not decoded sample values. FromISR queue calls
 * are the interrupt-safe versions: they never wait for another task to run.
 */
bool RecordingApplication::receiveBlock(const parlio_rx_event_data_t* event)
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
        if (copyUs > _maxCopyUs) { _maxCopyUs = copyUs; }

        // &block lets FreeRTOS copy the pointer value into the queue. It does
        // not copy the pointed-to bytes. CPU1 may start reading after this send.
        if (xQueueSendFromISR(_readyBlocks, &captureBlock, &awakened) != pdTRUE)
        {
            _droppedBlocks.fetch_add(1, std::memory_order_relaxed);
            xQueueSendFromISR(_freeBlocks, &captureBlock, &awakened);
        }

        // Count waiting blocks only, excluding a block already held by CPU1.
        const uint32_t queued = uxQueueMessagesWaitingFromISR(_readyBlocks);
        if (queued > _queueHighWater) { _queueHighWater = queued; }
    }
    return awakened == pdTRUE;
}

/**
 * @brief Core 1's permanent task waits for raw blocks and decodes them in order.
 * initialize() creates this task once; decodeTask forwards its context here. It never returns.
 * Only this task changes the decoder during capture. Core 0 may inspect or
 * reset it only after asking this task to finish its queued work.
 * A null pointer in readyBlocks means "everything before this must be finished."
 * After seeing it, this task gives the drained semaphore to signal completion.
 * Ordinary blocks return to freeBlocks only AFTER decoder.feed() reads them
 * and recordWords() copies or summarizes the decoded numbers. Even if an SD
 * submission fails, no later code retains the raw pointer, so it can be reused.
 * portMAX_DELAY sleeps this task until a pointer is available, rather than
 * burning CPU time checking repeatedly. It does not stop DMA or Core 0.
 * This task is woken through a queue; it is NOT an interrupt handler.
 */
void RecordingApplication::decode()
{
    int64_t lastPauseAt = esp_timer_get_time();
    bool firstBlock = true;
    for (;;)
    {
        CaptureBlock* captureBlock = nullptr;
        xQueueReceive(_readyBlocks, &captureBlock, portMAX_DELAY);
        if (captureBlock != nullptr)
        {
            // Timestamp before decoding so queue wait includes ISR copy and
            // scheduling delay. Timing reads add overhead only once per block.
            const int64_t startedAt = esp_timer_get_time();
            if (firstBlock) { lastPauseAt = startedAt; firstBlock = false; }

            // feed() reconstructs words and calls recordWords() synchronously.
            // That path copies samples into the SD ring or updates the RAM
            // summary. Neither waits for SD or retains this raw block.
            _frameDecoder.feed(captureBlock->_data, captureBlock->_length);
            const int64_t finishedAt = esp_timer_get_time();
            if (_recordingObserver != nullptr)
            { _recordingObserver->decoded(startedAt - captureBlock->_arrivedAt, finishedAt - startedAt,
                                           static_cast<uint32_t>(captureBlock->_length)); }

            // Return the ADDRESS, not the data: the memory stays in place and
            // Core 0 is now allowed to overwrite it with another DMA chunk.
            xQueueSend(_freeBlocks, &captureBlock, portMAX_DELAY);

            // Keep background/idle scheduling opportunities even under backlog,
            // but at most once per 100 ms rather than every sixteen blocks.
            // Reset the deadline on the first block of each run. Normal
            // empty-queue waits also let idle tasks run. vTaskDelay(1) waits
            // one operating-system tick, whose duration is a build setting;
            // the 100000 comparison is in microseconds, not ticks.
            if (finishedAt - lastPauseAt >= 100000)
            {
                const int64_t pauseStartedAt = esp_timer_get_time();
                vTaskDelay(1);
                lastPauseAt = esp_timer_get_time();
                if (_recordingObserver != nullptr) { _recordingObserver->paused(lastPauseAt - pauseStartedAt); }
            }
        }
        else
        {
            // The null marker follows all real blocks for the stopped run.
            // Signal Core 0 that decoder state and summaries can now be read.
            lastPauseAt = esp_timer_get_time();
            firstBlock = true;
            xSemaphoreGive(_drained);
        }
    }
}

/**
 * @brief Core 0 waits until Core 1 has finished all previously queued raw blocks.
 * Called after the receiver stops, both for normal shutdown and setup failure.
 * Send a null pointer behind the real blocks, then wait for the decoder's
 * completion signal. This prevents reading/resetting state while Core 1 is
 * changing it. Queue insertion waits without a timeout; the completion wait
 * allows one second. A missed completion sends FAULT and aborts via
 * ESP_ERROR_CHECK rather than proceeding with shared state still in use.
 */
void RecordingApplication::drainDecoder()
{
    CaptureBlock* captureBlockMarker = nullptr;
    xQueueSend(_readyBlocks, &captureBlockMarker, portMAX_DELAY);
    if (xSemaphoreTake(_drained, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        reply("ADC127 FAULT %lu reason=decoder-stalled\n", _activeRun);
        ESP_ERROR_CHECK(ESP_ERR_TIMEOUT);
    }
}

/**
 * @brief Core 0 stops receiving and frees this run's hardware receiver and DMA RAM.
 * Used by stopCapture() and failed setup. enabled and the non-null handles
 * identify which setup steps succeeded. Disable reception before deleting
 * driver objects or freeing memory, so hardware cannot write into freed RAM.
 * ESP_ERROR_CHECK aborts on a driver cleanup error. This does not stop the
 * ADC itself: Feather controls that. Private raw blocks and the decoder task
 * remain allocated for later runs; drainDecoder() waits for their current work.
 */
void RecordingApplication::releaseReceiver()
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

/**
 * @brief Set up the P4 to receive the requested test rate, before Feather starts the ADC.
 * handleLine() calls this on Core 0 after checking stage, an index into the
 * shared rate table. Reset previous statistics, allocate raw receive memory,
 * configure PARLIO, and give its driver the buffer to fill. GPIO0 receives
 * DCLK, GPIO2 receives DOUT0, and GPIO1 receives FSYNC; all three are inputs.
 * The frequency settings describe the expected external clock, not a clock
 * generated by the P4. The default free_clk=false permits gaps in that clock.
 * ESP_OK means the setup calls succeeded and receiving was requested, not
 * that ADC data arrived. On failure, release any receiver resources created
 * so far and wait for already queued decoding before returning the error.
 */
esp_err_t RecordingApplication::startCapture(unsigned stage)
{
    // Previous decoding must already be finished. A slot is 32 ADC clock
    // positions; some slower rates have unused slots between sample frames.
    // reset() tells the decoder how many clock positions to expect per frame.
    const auto& ads127BenchRate = ads127BenchRates[stage];
    _frameDecoder.reset(ads127BenchRate._slots * 32U);
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
    config.ext_clk_freq_hz = ads127BenchRate._samplesPerSecond * ads127BenchRate._slots * 32U;
    config.exp_clk_freq_hz = config.ext_clk_freq_hz;
    config.clk_in_gpio_num = GPIO_NUM_0;
    config.clk_out_gpio_num = GPIO_NUM_NC;
    config.valid_gpio_num = GPIO_NUM_NC;
    for (auto& pin : config.data_gpio_nums) { pin = GPIO_NUM_NC; }
    config.data_gpio_nums[0] = GPIO_NUM_2;
    config.data_gpio_nums[1] = GPIO_NUM_1;
    if (retVal == ESP_OK) { retVal = parlio_new_rx_unit(&config, &_receiver); }

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
        // Do not leave hardware using this run's memory after a setup failure.
        // Also finish any blocks already handed to Core 1 before allowing retry.
        releaseReceiver();
        drainDecoder();
    }
    else if (_recordingObserver != nullptr)
    { _recordingObserver->started(xTaskGetCurrentTaskHandle(), _decoderTaskHandle); }
    return retVal;
}

/**
 * @brief Core 0 stops reception, finishes queued decoding, and reports the run.
 * Called by handleLine() for Feather STOP or by app_main() on error/timeout.
 * kind is DONE for a requested stop, FAULT for software failure, TIMEOUT for
 * the receiver deadline without matching STOP. None implies ADC success.
 * The local receiver cannot directly stop ADC conversions: FAULT/TIMEOUT asks
 * Feather to do so. The incomplete hardware descriptor is not delivered on
 * disable, so counts exclude its tail and are not an exact generated total.
 * In RAM mode this function prints the summary after Core 1 has finished.
 * In SD mode recording.finish() asks the separate disk task to drain, verify,
 * and close the file; this function does not wait for SD SAVED or SD FAILED.
 */
void RecordingApplication::stopCapture(const char* kind)
{
    if (_recordingObserver != nullptr) { _recordingObserver->stopping(); }
    releaseReceiver();
    drainDecoder();
    _active = false;

    // Core 1 and the receive callback are now finished, so these totals cannot
    // change beneath us. Estimate the sample rate from bytes and elapsed time
    // between callbacks. Each raw byte represents four clock positions.
    // Exclude the first chunk, which arrived before the measured interval.
    const CaptureStats& captureStats = _frameDecoder.stats();
    const int64_t interval = _lastBlockAt - _firstBlockAt;
    const double observedRate = interval > 0 ?
        (_rawBytes - _firstBlockBytes) * 4.0e6 /
        (interval * ads127BenchRates[_activeStage]._slots * 32.0) : 0.0;
    reply("ADC127 %s %lu words=%lu bad=%lu periodErrors=%lu short=%lu drops=%lu errors=%lu rate=%.1f/s\n",
          kind, _activeRun, static_cast<unsigned long>(captureStats._words),
          static_cast<unsigned long>(captureStats._badHeaders),
          static_cast<unsigned long>(captureStats._badPeriods),
          static_cast<unsigned long>(captureStats._shortWords),
          static_cast<unsigned long>(_droppedBlocks.load()),
          static_cast<unsigned long>(_callbackErrors.load()), observedRate);

    // USB-only details follow the short Feather reply, after capture has stopped.
    // Rate is estimated between DMA completion timestamps, not by counting CPU
    // loop iterations. Exclude the first block because it preceded the interval.
    printf("stage=%u requested=%lu/s bytes=%llu blocks=%lu queueHigh=%lu/%u "
           "tailBits=%lu raw=%08lx sample=%ld\n", _activeStage,
           static_cast<unsigned long>(ads127BenchRates[_activeStage]._samplesPerSecond),
           static_cast<unsigned long long>(_rawBytes), static_cast<unsigned long>(_callbacks),
           static_cast<unsigned long>(_queueHighWater), _blockCount,
           static_cast<unsigned long>(_frameDecoder.pendingBits()),
           static_cast<unsigned long>(captureStats._latestWord), static_cast<long>(captureStats._latestSample));

    if (_recordingObserver != nullptr)
    { _recordingObserver->stopped(ads127BenchRates[_activeStage]._samplesPerSecond,
        ads127BenchRates[_activeStage]._slots, _maxCallbackGapUs, _maxCopyUs); }
    // "header" here is the ADC's eight-bit status value, not the file header.
    // Print counts for every observed value, plus up to eight suspect words
    // for diagnosis. This output happens only after reception has stopped.
    for (unsigned header = 0; header < 256; ++header)
    {
        if (captureStats._headers[header] != 0)
        {
            printf("header=%02x count=%lu\n", header,
                   static_cast<unsigned long>(captureStats._headers[header]));
        }
    }
    for (unsigned index = 0; index < 8 && index < captureStats._badHeaders; ++index)
    {
        printf("badWord[%u]=%08lx\n", index,
               static_cast<unsigned long>(captureStats._badExamples[index]));
    }
    printf("DONE means receiver stopped. Missing final DMA tail and undetected "
           "hardware loss remain possible; review counts and errors.\n");

    // The protocol acknowledgement precedes potentially slow SD draining and
    // readback. The disk task prints SD SAVED separately when those finish.
    // A bad header does not itself stop capture early. After draining, even ONE
    // counted bad header invalidates this test recording. SD still preserves
    // and verifies the payload, but reports FAILED and writes an invalid header.
    // This distinguishes bad acquisition from a disk write failure; see TODO.md
    // for the undecided production policy. Power/saturation flags alone are not
    // badHeaders: frameDecoder::finishWord() documents the exact mask.
    const bool clean = strcmp(kind, "DONE") == 0 && captureStats._words != 0 &&
        captureStats._badHeaders == 0 && captureStats._badPeriods == 0 && captureStats._shortWords == 0 &&
        _droppedBlocks.load() == 0 && _callbackErrors.load() == 0 && !_frameDecoder.outputFailed();
    _captureDestination->finish(_activeRun, ads127BenchRates[_activeStage]._samplesPerSecond,
                                clean, captureStats._words);
}

/**
 * @brief Handle one complete command line from Feather, on Core 0.
 * app_main() assembles incoming characters and passes a null-terminated line.
 * ARM2 supplies a nonzero run ID and a stage (sampling-rate table index).
 * The run ID ties replies and STOP to the correct test. The extra %c in the
 * scans detects unexpected text after the required arguments.
 * Repeating the same active ARM2 resends READY2 without clearing data or
 * extending its timeout. A different request receives BUSY. A new SD capture
 * must claim a previously prepared file; RAM mode needs no file.
 * STOP acts only when its ID matches the remembered run. If already stopped,
 * it repeats DONE without redoing cleanup. Other lines are silently ignored.
 */
void RecordingApplication::handleLine(const char* line)
{
    // The scan must find exactly the required arguments. extra is used only
    // to detect unwanted trailing text, not as a command argument.
    unsigned long run = 0;
    unsigned stage = 0;
    char extra = 0;
    if (sscanf(line, "ADC127 ARM2 %lu %u %c", &run, &stage, &extra) == 2)
    {
        if (_active)
        {
            if (run == _activeRun && stage == _activeStage)
            {
                reply("ADC127 READY2 %lu %u\n", run, stage);
            }
            else { reply("ADC127 BUSY %lu\n", run); }
        }
        else if (run == 0 || stage >= ads127BenchRateCount)
        {
            reply("ADC127 FAILED %lu reason=invalid-request\n", run);
        }
        else if (!_captureDestination->claim(run, stage, ads127BenchRates[stage]._samplesPerSecond))
        {
            reply("ADC127 FAILED %lu reason=SD-not-ready-press-p-on-P4\n", run);
        }
        else
        {
            // Remember this run before starting hardware so failures can be
            // reported with its ID. RAM summaries reset here; SD preparation
            // was already claimed above. Neither operation starts the ADC.
            _activeRun = run;
            _activeStage = stage;
            _modeLocked = true;
            _stopRequestedAt = 0;
            const esp_err_t result = startCapture(stage);
            if (result == ESP_OK)
            {
                // Start the receiver deadline and grant permission to Feather.
                // We do not wait here for the first ADC sample to arrive.
                _armedAt = esp_timer_get_time();
                _active = true;
                reply("ADC127 READY2 %lu %u\n", run, stage);
            }
            else
            {
                // startCapture() has already stopped/cleaned up the receiver.
                // Ask the SD task to finish this claimed file as failed.
                reply("ADC127 FAILED %lu reason=%s\n", run, esp_err_to_name(result));
                _captureDestination->finish(_activeRun, ads127BenchRates[stage]._samplesPerSecond, false, 0);
            }
        }
    }
    else if (sscanf(line, "ADC127 STOP %lu %c", &run, &extra) == 1 && run == _activeRun)
    {
        if (_active) { stopCapture("DONE"); }
        else { reply("ADC127 DONE %lu\n", run); }
    }
}

/**
 * @brief Give a batch of decoded numbers to the selected SD or RAM destination.
 * The decoder calls this on Core 1, during decoder.feed(). words points to
 * count 32-bit values: each has 8 status bits and a 24-bit ADC sample. count
 * is a number of words, not bytes. The context argument identifies this permanent application instance.
 * The decoder checks status before this call. In SD mode submitSamples()
 * removes status, packs each sample into three bytes, and copies them into
 * the recording ring. True means queued in RAM, not saved to the card.
 * False records an output failure in the decoder. In RAM mode accept()
 * updates the summary immediately and this function returns true, including
 * while skipping startup samples and ignoring samples after the summary fills.
 * Neither destination retains words: the decoder can reuse its batch on return.
 */
bool RecordingApplication::recordWords(const uint32_t* words, size_t count, void* context)
{
    return static_cast<RecordingApplication*>(context)->_captureDestination->submit(words, count);
}

/**
 * @brief Allocate receiver resources and open the two command connections once.
 * The small startup program calls this on Core 0 after initializing storage.
 * It returns after creating the permanent Core 1 decoder task and opening UART1
 * at 115200 bits/s (RX GPIO4, TX GPIO5). No card operation or capture starts here.
 * Failed required setup aborts through ESP_ERROR_CHECK rather than continuing
 * with missing resources. Startup subsequently calls poll() to process commands.
 */
void RecordingApplication::initialize()
{
    // Each completed decoder batch goes through this application's selected
    // destination. The callback borrows this permanent application instance.
    _frameDecoder.setOutput(recordWords, this);

    // Nonblocking console reads let the UART control task service Feather even
    // when Karl has not typed anything. Only 'p' prepares a new card file.
    const int consoleFlags = fcntl(STDIN_FILENO, F_GETFL, 0);
    ESP_ERROR_CHECK(consoleFlags >= 0 && fcntl(STDIN_FILENO, F_SETFL,
        consoleFlags | O_NONBLOCK) == 0 ? ESP_OK : ESP_FAIL);

    // GPIO means a general-purpose input/output pin. Configure pins 0, 1, 2
    // as inputs only. Zero-initialization leaves internal pull-up/pull-down
    // resistors disabled; the external voltage translator supplies the levels.
    gpio_config_t inputs = {};
    inputs.pin_bit_mask = (1ULL << 0) | (1ULL << 1) | (1ULL << 2);
    inputs.mode = GPIO_MODE_INPUT;
    ESP_ERROR_CHECK(gpio_config(&inputs));

    // Allocate all raw blocks before capture: no allocation inside the ISR.
    // A queue item is only one pointer. Seventeen ready slots permit a null
    // shutdown marker after all 16 real blocks; there is NO seventeenth block.
    // drained is a semaphore: a completion signal from CPU1 back to CPU0 so
    // stopCapture can safely inspect/reset decoder state after queued work ends.
    _captureBlocks = static_cast<CaptureBlock*>(heap_caps_calloc(
        _blockCount, sizeof(CaptureBlock), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    _freeBlocks = xQueueCreate(_blockCount, sizeof(CaptureBlock*));
    _readyBlocks = xQueueCreate(_blockCount + 1, sizeof(CaptureBlock*));
    _drained = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(_captureBlocks && _freeBlocks && _readyBlocks && _drained ? ESP_OK : ESP_ERR_NO_MEM);

    // Put each block's address in the free queue once. &blocks[index] selects
    // that block in the array; &block gives the queue the address of the local
    // pointer so it can copy its VALUE. Queue operations do not copy raw data.
    for (unsigned index = 0; index < _blockCount; ++index)
    {
        CaptureBlock* captureBlock = &_captureBlocks[index];
        xQueueSend(_freeBlocks, &captureBlock, 0);
    }

    // Initially every block is free. Pin the decoder to CPU1, with a 4096-byte
    // task stack and scheduling priority 10. Stack storage is for function calls
    // and locals; it is separate from the sixteen 4096-byte input blocks.
    TaskHandle_t* decoderHandleOutput = &_decoderTaskHandle;
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(decodeTask, "decode", 4096, this, 10,
                                          decoderHandleOutput, 1) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    // Set up Feather's serial link: eight data bits, no parity bit, one stop
    // bit, and no extra flow-control wires. The driver stores up to 2048 RX
    // (received) bytes and 512 TX (transmit) bytes and queues 16 event reports.
    // Framing errors mean invalid serial framing; overflow means incoming
    // bytes were lost. The loop below discards affected input and faults an
    // active capture rather than acting on a potentially damaged command.
    uart_config_t uartConfig = {};
    uartConfig.baud_rate = 115200;
    uartConfig.data_bits = UART_DATA_8_BITS;
    uartConfig.parity = UART_PARITY_DISABLE;
    uartConfig.stop_bits = UART_STOP_BITS_1;
    uartConfig.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uartConfig.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_param_config(_featherPort, &uartConfig));
    ESP_ERROR_CHECK(uart_set_pin(_featherPort, 5, 4, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(_featherPort, 2048, 512, 16, &_uartEvents, 0));

    printf("P4 receiver ready for commands. Press p and wait for SD READY. DCLK=0 FSYNC=1 DOUT0=2 RX=4 TX=5.\n");
    printf("ADC files: 3 bytes/sample, status still checked; stage 6 requires 24.576 Mbps to SD.\n");
}

/**
 * @brief Service console input, Feather commands and capture faults once on Core 0.
 * Startup calls repeatedly. Each pass yields for one OS tick; it does not wait
 * for console input. Only prepare requests disk work; startup itself never does.
 */
void RecordingApplication::poll()
{
    char consoleKey = 0;
    if (read(STDIN_FILENO, &consoleKey, 1) == 1)
    {
        if (consoleKey == 'p')
        {
            _modeLocked = true;
            const bool accepted = !_active && _captureDestination->prepare();
            printf("SD prepare %s\n", accepted ? "requested; wait for SD READY" : "busy/already prepared");
        }
        else if (_recordingObserver != nullptr) { _recordingObserver->command(*this, consoleKey); }
    }
    // First collect serial error reports. Timeout 0 means each queue read
    // returns immediately. A bad link clears both the driver's unread
    // characters and our partly assembled line; during capture it is fatal
    // to that run and is included in the reported errors count.
    uart_event_t event = {};
    bool linkError = false;
    while (xQueueReceive(_uartEvents, &event, 0) == pdTRUE)
    {
        if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
            event.type == UART_PARITY_ERR || event.type == UART_FRAME_ERR)
        {
            linkError = true;
        }
    }
    if (linkError)
    {
        uart_flush_input(_featherPort);
        _benchLine = BenchLine{};
        if (_active)
        {
            _callbackErrors.fetch_add(1);
            stopCapture("FAULT");
        }
    }
    // uart_read_bytes() actually copies Feather's incoming characters to
    // this array, up to 256 per pass without waiting. benchLine collects
    // them until newline and rejects oversized lines; feed() returns true
    // only when handleLine() has a complete command to examine.
    uint8_t incoming[256];
    const int received = uart_read_bytes(_featherPort, incoming, sizeof(incoming), 0);
    for (int index = 0; index < received; ++index)
    {
        if (_benchLine.feed(static_cast<char>(incoming[index]))) { handleLine(_benchLine.value()); }
    }
    if (_active)
    {
        // A completed RAM window asks the Feather to stop the ADC. Its
        // matching STOP returns through handleLine(), then we dump USB data.
        // Older Feather firmware ignores RAMFULL: bound that wait to 2 s
        // and report FAULT (which also asks older firmware to stop).
        if (_captureDestination->full() && _stopRequestedAt == 0)
        {
            _stopRequestedAt = esp_timer_get_time();
            reply("ADC127 RAMFULL %lu\n", _activeRun);
        }
        // Stop on lost private blocks, invalid receive chunks, SD failure,
        // or a RAM stop request unanswered for two seconds. Independently,
        // limit total time since ARM to five seconds in RAM mode or the
        // shared test timeout in SD mode. esp_timer_get_time() is in
        // microseconds; multiply millisecond limits by 1000 for comparison.
        if (_droppedBlocks.load() != 0 || _callbackErrors.load() != 0 ||
            _captureDestination->failed() ||
            (_stopRequestedAt != 0 && esp_timer_get_time() - _stopRequestedAt > 2000000))
        {
            stopCapture("FAULT");
        }
        else if (esp_timer_get_time() - _armedAt >=
                 static_cast<int64_t>(_timeoutMs) * 1000)
        {
            stopCapture("TIMEOUT");
        }
        if (_active && _recordingObserver != nullptr) { _recordingObserver->poll(); }
    }
    // Yield according to the OS tick setting rather than continuously
    // checking commands. This requests a one-millisecond delay converted
    // to ticks; it does not promise a precise one-millisecond loop period.
    vTaskDelay(pdMS_TO_TICKS(1));
}
