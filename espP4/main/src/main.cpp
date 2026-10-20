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
 * @file main.cpp
 * @brief Receive measurements from the ADS127L18 and save them to SD or inspect them in RAM.
 *
 * This is the P4 program. The Feather sets up the ADC (analog-to-digital
 * converter) and tells it when to start and stop measuring. The P4 receives
 * the resulting numbers. It does not control the analog test voltage or
 * configure the ADC, and powering it up does not start a recording.
 *
 * There are two ways to use it:
 * - SD recording: type 'p' in the P4's PC terminal, wait for SD READY, then
 *   start a test from the Feather. Every decoded sample is stored as three
 *   bytes, without averaging. SD SAVED is the later storage result; a DONE
 *   message from the receiver does not mean the file has finished saving.
 * - RAM inspection: type 'w' before requesting SD preparation or capture.
 *   The same receiver and decoder run, but samples go to a waveform summary
 *   instead of a file. Skip the first second of decoded samples, then collect
 *   300 ms in 600 groups of 0.5 ms each. Keep each group's minimum, maximum,
 *   and sum; print its average when reporting. At stage 6 that is 512 samples
 *   per group. This is a summary, not a copy of every individual sample.
 *   The card is not accessed, although the SD task and its RAM still exist.
 *   Reset the P4 before changing between these modes.
 *
 * Start reading at app_main(), near the end of this file. ESP-IDF, the P4's
 * software framework, calls it after boot. It allocates memory, starts the
 * background tasks, then repeatedly checks commands and capture errors.
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
 * Read sdRecorder.cpp here for the actual card writes and
 * test/hardware/ramWaveform.cpp (under espP4) for the summary calculations. The hardware and memory details follow below.
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
#include "ads127BenchRate.h"
#include "test/support/ads127BenchTiming.h"
#include "test/support/benchLine.h"
#include "frameDecoder.h"
#include "sdRecorder.h"
#include "decoderTiming.h"
#include "sdWriteTest.h"
#include "ramWaveform.h"
#include "sdkconfig.h"
#if CONFIG_P4_CAPTURE_PROFILE
#include "captureProfile.h"
#endif

#if !CONFIG_ESP32P4_SELECTS_REV_LESS_V3 || !CONFIG_ESPTOOLPY_FLASHSIZE_16MB
#error "This bench project requires P4 pre-v3 silicon support and 16 MB flash."
#endif

#if !CONFIG_SPIRAM
#error "SD recording needs PSRAM: build with sdkconfig.sd.defaults as documented."
#endif

static constexpr uart_port_t featherPort = UART_NUM_1;

// These are tested starting allocations, not proven minimum/optimal sizes.
// 32 KiB holds 4 ms of raw stage-6 input. It buys time to COPY completed chunks
// before DMA reuses them. A larger decoder queue cannot rescue a missed copy.
// IDF 5.5.6 with our aligned buffer and 64-byte cache alignment divides this into
// seven 4032-byte chunks, then 2240 and 2304 bytes: nine chunks, NOT eight 4 KiB
// chunks. See parlio_rx_mount_transaction_buffer(). This is driver-specific.
// Four DCLKs per byte gives 16128, 8960, 9216 clocks respectively, or 492.19,
// 273.44, 281.25 us at stage 6. Nine receive callbacks per 4 ms average 2250/s;
// callback service can be delayed after the hardware completion event.
static constexpr size_t dmaBytes = 32768;

// Private blocks must hold any delivered chunk. Sixteen 4 KiB data arrays use
// 64 KiB of internal RAM plus metadata/queue overhead. Their nominal capacity
// is 8 ms of raw input; partly filled blocks and blocks being copied/decoded
// reduce the waiting capacity. This cushions brief scheduling delays, not a
// decoder that is persistently slower than input. The early decoder exhausted
// this pool; optimizing decoding allowed passing without increasing these sizes.
static constexpr size_t blockBytes = 4096;
static constexpr unsigned blockCount = 16;
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "ISR counters must not invoke a locking implementation");

/**
 * @brief One raw block copied by Core 0 and retained until Core 1 finishes decoding it.
 * length is the number of valid bytes, not the array capacity. arrivedAt is
 * the interrupt callback's timestamp in microseconds since boot; it lets the
 * decoder measure how long the block waited before being processed.
 */
struct captureBlock
{
    size_t length;
    int64_t arrivedAt;
    uint8_t data[blockBytes];
};

static decoderTiming timing;

static captureBlock* blocks = nullptr;

// Both FreeRTOS queues hold captureBlock* addresses into the SAME blocks array.
// freeBlocks means Core 0 may overwrite the block; readyBlocks means Core 1
// must read it first. The queues are not another 64 KiB copy of the data.
static QueueHandle_t freeBlocks = nullptr;
static QueueHandle_t readyBlocks = nullptr;
// UART is the serial link between boards. Its event queue reports receive
// errors separately from command bytes. drained is a semaphore, a completion
// signal Core 1 gives when it has finished all previously queued raw blocks.
static QueueHandle_t uartEvents = nullptr;
static SemaphoreHandle_t drained = nullptr;
static frameDecoder decoder;
#if CONFIG_P4_CAPTURE_PROFILE
// Core 0 alone owns profiling state. No extra task, ISR call or decoder-loop
// instrumentation is added. Runtime-counter bookkeeping is an SDK build option.
static captureProfile profile;
static TaskHandle_t decoderTaskHandle = nullptr;
#endif
static sdRecorder recording;
static sdWriteTest storageTest;
// Mode is selected on Core 0 before any capture/SD commands after boot and is
// then fixed until reset. Core 1 reads it only after receiving a queued block.
static bool ramMode = false;
static bool modeLocked = false;
static ramWaveform waveform;
static int64_t ramStopRequestedAt = 0;
static parlio_rx_unit_handle_t receiver = nullptr;
static parlio_rx_delimiter_handle_t delimiter = nullptr;
static uint8_t* dmaBuffer = nullptr;
// enabled tracks whether the hardware receiver must be disabled during cleanup.
// active means a capture request was accepted and has not yet been stopped;
// it does not mean the ADC has actually supplied any samples.
static bool enabled = false;
static bool active = false;
static unsigned activeStage = 0;
static unsigned long activeRun = 0;
// armedAt starts the capture timeout when setup succeeds, before Feather starts
// the ADC. Atomic counters can be safely updated/read across the interrupt and
// control code without a separate lock. callbackErrors also includes UART errors
// detected during capture, despite its narrower historical name.
static int64_t armedAt = 0;
static std::atomic<uint32_t> droppedBlocks{0};
static std::atomic<uint32_t> callbackErrors{0};

// The ISR alone writes these measurements. CPU0 reads them only after disabling
// the receiver on CPU0, where its interrupt was installed, and draining CPU1.
// More than 14 GB passes through DMA in 30 minutes. ISR owns this 64-bit
// total exclusively; CPU0 reads it only after disabling capture, never live.
static uint64_t rawBytes = 0;
static uint32_t firstBlockBytes = 0;
static uint32_t callbacks = 0;
static int64_t firstBlockAt = 0;
static int64_t lastBlockAt = 0;
static uint32_t queueHighWater = 0;
static int64_t maxCallbackGapUs = 0;
static int64_t maxCopyUs = 0;

/**
 * @brief Core 0 formats a control reply for Feather and prints the same text on the PC.
 * Callers supply printf-style format text and values. Replies describe setup,
 * stop requests, or results; READY2 and RAMFULL can be sent during an active
 * capture. This function is never called from the receive interrupt handler.
 * uart_write_bytes() queues text for UART1 transmission on GPIO5; printf()
 * writes the PC console. Neither is confirmation that Feather received it.
 * Formatting failure or a line too large for the 384-byte array sends nothing.
 */
static void reply(const char* format, ...)
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
        uart_write_bytes(featherPort, message, length);
        printf("%s", message);
    }
}

/**
 * @brief The receive driver calls this on Core 0 when a raw DMA chunk is ready.
 * It runs inside the driver's ISR (Interrupt Service Routine), not a normal
 * task. event describes the delivered bytes. The unused arguments are the
 * driver's receiver handle and optional caller context.
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
static bool receiveBlock(parlio_rx_unit_handle_t,
                         const parlio_rx_event_data_t* event, void*)
{
    // These timestamps measure when software handles completed chunks, not
    // individual ADC sample times. rawBytes counts driver-reported bytes,
    // including a chunk that the checks below might reject.
    BaseType_t awakened = pdFALSE;
    const int64_t now = esp_timer_get_time();
    if (callbacks == 0)
    {
        firstBlockAt = now;
        firstBlockBytes = static_cast<uint32_t>(event->recv_bytes);
    }
    else if (now - lastBlockAt > maxCallbackGapUs)
    {
        maxCallbackGapUs = now - lastBlockAt;
    }
    lastBlockAt = now;
    ++callbacks;
    rawBytes += static_cast<uint32_t>(event->recv_bytes);

    // Reject invalid data or lack of private storage without waiting. Atomic
    // counters let the main loop notice the problem and stop the run later.
    captureBlock* block = nullptr;
    if (event->data == nullptr || event->recv_bytes == 0 || event->recv_bytes > blockBytes)
    {
        callbackErrors.fetch_add(1, std::memory_order_relaxed);
    }
    else if (xQueueReceiveFromISR(freeBlocks, &block, &awakened) != pdTRUE)
    {
        droppedBlocks.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        // The free queue gave us exclusive use of this block. Copy the actual
        // received length, not its 4096-byte capacity (chunks can be shorter).
        block->length = event->recv_bytes;
        block->arrivedAt = now;
        memcpy(block->data, event->data, block->length);
        const int64_t copyUs = esp_timer_get_time() - now;
        if (copyUs > maxCopyUs) { maxCopyUs = copyUs; }

        // &block lets FreeRTOS copy the pointer value into the queue. It does
        // not copy the pointed-to bytes. CPU1 may start reading after this send.
        if (xQueueSendFromISR(readyBlocks, &block, &awakened) != pdTRUE)
        {
            droppedBlocks.fetch_add(1, std::memory_order_relaxed);
            xQueueSendFromISR(freeBlocks, &block, &awakened);
        }

        // Count waiting blocks only, excluding a block already held by CPU1.
        const uint32_t queued = uxQueueMessagesWaitingFromISR(readyBlocks);
        if (queued > queueHighWater) { queueHighWater = queued; }
    }
    return awakened == pdTRUE;
}

/**
 * @brief Core 1's permanent task waits for raw blocks and decodes them in order.
 * app_main() creates this task once with no argument. It never returns.
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
static void decodeTask(void*)
{
    int64_t lastPauseAt = esp_timer_get_time();
    for (;;)
    {
        captureBlock* block = nullptr;
        xQueueReceive(readyBlocks, &block, portMAX_DELAY);
        if (block != nullptr)
        {
            // Timestamp before decoding so queue wait includes ISR copy and
            // scheduling delay. Timing reads add overhead only once per block.
            const int64_t startedAt = esp_timer_get_time();
            if (timing.blocks == 0) { lastPauseAt = startedAt; }
            timing.recordWait(startedAt - block->arrivedAt);

            // feed() reconstructs words and calls recordWords() synchronously.
            // That path copies samples into the SD ring or updates the RAM
            // summary. Neither waits for SD or retains this raw block.
            decoder.feed(block->data, block->length);
            const int64_t finishedAt = esp_timer_get_time();
            timing.recordDecode(finishedAt - startedAt, static_cast<uint32_t>(block->length));

            // Return the ADDRESS, not the data: the memory stays in place and
            // Core 0 is now allowed to overwrite it with another DMA chunk.
            xQueueSend(freeBlocks, &block, portMAX_DELAY);

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
                timing.recordPause(lastPauseAt - pauseStartedAt);
            }
        }
        else
        {
            // The null marker follows all real blocks for the stopped run.
            // Signal Core 0 that decoder state and summaries can now be read.
            lastPauseAt = esp_timer_get_time();
            xSemaphoreGive(drained);
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
static void drainDecoder()
{
    captureBlock* marker = nullptr;
    xQueueSend(readyBlocks, &marker, portMAX_DELAY);
    if (xSemaphoreTake(drained, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        reply("ADC127 FAULT %lu reason=decoder-stalled\n", activeRun);
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
static void releaseReceiver()
{
    if (enabled)
    {
        ESP_ERROR_CHECK(parlio_rx_unit_disable(receiver));
        enabled = false;
    }
    if (receiver != nullptr)
    {
        ESP_ERROR_CHECK(parlio_del_rx_unit(receiver));
        receiver = nullptr;
    }
    if (delimiter != nullptr)
    {
        ESP_ERROR_CHECK(parlio_del_rx_delimiter(delimiter));
        delimiter = nullptr;
    }
    heap_caps_free(dmaBuffer);
    dmaBuffer = nullptr;
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
static esp_err_t startCapture(unsigned stage)
{
    // Previous decoding must already be finished. A slot is 32 ADC clock
    // positions; some slower rates have unused slots between sample frames.
    // reset() tells the decoder how many clock positions to expect per frame.
    const auto& rate = ads127BenchRates[stage];
    decoder.reset(rate.slots * 32U);
    timing = {};
    droppedBlocks.store(0);
    callbackErrors.store(0);
    rawBytes = firstBlockBytes = callbacks = queueHighWater = 0;
    firstBlockAt = lastBlockAt = 0;
    maxCallbackGapUs = maxCopyUs = 0;

    // Allocate the hardware's raw buffer with 64-byte address alignment for
    // the driver/cache requirements. Capability flags request internal RAM
    // usable by DMA and accessible byte by byte. A null pointer is a setup
    // failure; subsequent driver steps run only while retVal remains ESP_OK.
    dmaBuffer = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        64, dmaBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    esp_err_t retVal = dmaBuffer != nullptr ? ESP_OK : ESP_ERR_NO_MEM;

    // Reserve one receive transaction, repeatedly using the same raw buffer.
    // data_width=2 captures DOUT0 and FSYNC together; the ADC supplies the
    // clock. dma_burst_size=16 requests 16-byte hardware transfer bursts, not
    // 16-byte ADC samples. NC means "not connected": unused pins are unassigned.
    parlio_rx_unit_config_t config = {};
    config.trans_queue_depth = 1;
    config.max_recv_size = dmaBytes;
    config.dma_burst_size = 16;
    config.data_width = 2;
    config.clk_src = PARLIO_CLK_SRC_EXTERNAL;
    config.ext_clk_freq_hz = rate.samplesPerSecond * rate.slots * 32U;
    config.exp_clk_freq_hz = config.ext_clk_freq_hz;
    config.clk_in_gpio_num = GPIO_NUM_0;
    config.clk_out_gpio_num = GPIO_NUM_NC;
    config.valid_gpio_num = GPIO_NUM_NC;
    for (auto& pin : config.data_gpio_nums) { pin = GPIO_NUM_NC; }
    config.data_gpio_nums[0] = GPIO_NUM_2;
    config.data_gpio_nums[1] = GPIO_NUM_1;
    if (retVal == ESP_OK) { retVal = parlio_new_rx_unit(&config, &receiver); }

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
    delimiterConfig.eof_data_len = dmaBytes;
    if (retVal == ESP_OK)
    {
        retVal = parlio_new_rx_soft_delimiter(&delimiterConfig, &delimiter);
    }
    // Register the function the driver calls for each completed raw chunk.
    // Enabling with true clears the driver's previous receive requests.
    // Keep enabled accurate so cleanup also works after a later setup failure.
    parlio_rx_event_callbacks_t events = {};
    events.on_partial_receive = receiveBlock;
    if (retVal == ESP_OK)
    {
        retVal = parlio_rx_unit_register_event_callbacks(receiver, &events, nullptr);
    }
    if (retVal == ESP_OK)
    {
        retVal = parlio_rx_unit_enable(receiver, true);
        enabled = retVal == ESP_OK;
    }
    if (retVal == ESP_OK)
    {
        retVal = parlio_rx_soft_delimiter_start_stop(receiver, delimiter, true);
    }
    // partial_rx_en selects continuous reuse of the raw buffer and callbacks
    // for completed pieces. receive() is the call that hands dmaBuffer to
    // the driver; it does not wait for all the incoming data to arrive.
    parlio_receive_config_t receiveConfig = {};
    receiveConfig.delimiter = delimiter;
    receiveConfig.flags.partial_rx_en = true;
    if (retVal == ESP_OK)
    {
        retVal = parlio_rx_unit_receive(receiver, dmaBuffer, dmaBytes, &receiveConfig);
    }
    if (retVal != ESP_OK)
    {
        // Do not leave hardware using this run's memory after a setup failure.
        // Also finish any blocks already handed to Core 1 before allowing retry.
        releaseReceiver();
        drainDecoder();
    }
#if CONFIG_P4_CAPTURE_PROFILE
    else
    {
        // Optional CPU-usage measurement starts after receiver setup. In SD
        // mode file preparation is already over. READY2 and ADC START follow,
        // so their short exchange is included in the measured interval.
        profile.begin(xTaskGetCurrentTaskHandle(), decoderTaskHandle, recording.profileTask());
    }
#endif
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
static void stopCapture(const char* kind)
{
#if CONFIG_P4_CAPTURE_PROFILE
    // Freeze before teardown/draining: file verification must not inflate CPU
    // usage attributed to steady acquisition. Reporting is deferred until quiet.
    profile.finish();
#endif
    releaseReceiver();
    drainDecoder();
    active = false;

    // Core 1 and the receive callback are now finished, so these totals cannot
    // change beneath us. Estimate the sample rate from bytes and elapsed time
    // between callbacks. Each raw byte represents four clock positions.
    // Exclude the first chunk, which arrived before the measured interval.
    const captureStats& stats = decoder.stats();
    const int64_t interval = lastBlockAt - firstBlockAt;
    const double observedRate = interval > 0 ?
        (rawBytes - firstBlockBytes) * 4.0e6 /
        (interval * ads127BenchRates[activeStage].slots * 32.0) : 0.0;
    reply("ADC127 %s %lu words=%lu bad=%lu periodErrors=%lu short=%lu drops=%lu errors=%lu rate=%.1f/s\n",
          kind, activeRun, static_cast<unsigned long>(stats.words),
          static_cast<unsigned long>(stats.badHeaders),
          static_cast<unsigned long>(stats.badPeriods),
          static_cast<unsigned long>(stats.shortWords),
          static_cast<unsigned long>(droppedBlocks.load()),
          static_cast<unsigned long>(callbackErrors.load()), observedRate);

    // USB-only details follow the short Feather reply, after capture has stopped.
    // Rate is estimated between DMA completion timestamps, not by counting CPU
    // loop iterations. Exclude the first block because it preceded the interval.
    printf("stage=%u requested=%lu/s bytes=%llu blocks=%lu queueHigh=%lu/%u "
           "tailBits=%lu raw=%08lx sample=%ld\n", activeStage,
           static_cast<unsigned long>(ads127BenchRates[activeStage].samplesPerSecond),
           static_cast<unsigned long long>(rawBytes), static_cast<unsigned long>(callbacks),
           static_cast<unsigned long>(queueHighWater), blockCount,
           static_cast<unsigned long>(decoder.pendingBits()),
           static_cast<unsigned long>(stats.latestWord), static_cast<long>(stats.latestSample));

    timing.report(ads127BenchRates[activeStage].samplesPerSecond,
                  ads127BenchRates[activeStage].slots, maxCallbackGapUs, maxCopyUs);
    // "header" here is the ADC's eight-bit status value, not the file header.
    // Print counts for every observed value, plus up to eight suspect words
    // for diagnosis. This output happens only after reception has stopped.
    for (unsigned header = 0; header < 256; ++header)
    {
        if (stats.headers[header] != 0)
        {
            printf("header=%02x count=%lu\n", header,
                   static_cast<unsigned long>(stats.headers[header]));
        }
    }
    for (unsigned index = 0; index < 8 && index < stats.badHeaders; ++index)
    {
        printf("badWord[%u]=%08lx\n", index,
               static_cast<unsigned long>(stats.badExamples[index]));
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
#if CONFIG_P4_CAPTURE_PROFILE
    profile.report();
#endif
    const bool clean = strcmp(kind, "DONE") == 0 && stats.words != 0 &&
        stats.badHeaders == 0 && stats.badPeriods == 0 && stats.shortWords == 0 &&
        droppedBlocks.load() == 0 && callbackErrors.load() == 0 && !decoder.outputFailed();
    if (ramMode)
    {
        // drainDecoder() above transfers ownership back to Core 0. Print only
        // now: USB output must never slow the running decode/interrupt path.
        // 'clean' concerns reported capture faults, not sine shape or clipping.
        waveform.report(activeRun, ads127BenchRates[activeStage].samplesPerSecond, clean);
    }
    else { recording.finish(clean, stats.words); }
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
static void handleLine(const char* line)
{
    // The scan must find exactly the required arguments. extra is used only
    // to detect unwanted trailing text, not as a command argument.
    unsigned long run = 0;
    unsigned stage = 0;
    char extra = 0;
    if (sscanf(line, "ADC127 ARM2 %lu %u %c", &run, &stage, &extra) == 2)
    {
        if (active)
        {
            if (run == activeRun && stage == activeStage)
            {
                reply("ADC127 READY2 %lu %u\n", run, stage);
            }
            else { reply("ADC127 BUSY %lu\n", run); }
        }
        else if (run == 0 || stage >= ads127BenchRateCount)
        {
            reply("ADC127 FAILED %lu reason=invalid-request\n", run);
        }
        else if (!ramMode && !recording.claim(run, stage, ads127BenchRates[stage].samplesPerSecond))
        {
            reply("ADC127 FAILED %lu reason=SD-not-ready-press-p-on-P4\n", run);
        }
        else
        {
            // Remember this run before starting hardware so failures can be
            // reported with its ID. RAM summaries reset here; SD preparation
            // was already claimed above. Neither operation starts the ADC.
            activeRun = run;
            activeStage = stage;
            modeLocked = true;
            ramStopRequestedAt = 0;
            if (ramMode) { waveform.reset(ads127BenchRates[stage].samplesPerSecond); }
            const esp_err_t result = startCapture(stage);
            if (result == ESP_OK)
            {
                // Start the receiver deadline and grant permission to Feather.
                // We do not wait here for the first ADC sample to arrive.
                armedAt = esp_timer_get_time();
                active = true;
                reply("ADC127 READY2 %lu %u\n", run, stage);
            }
            else
            {
                // startCapture() has already stopped/cleaned up the receiver.
                // Ask the SD task to finish this claimed file as failed.
                reply("ADC127 FAILED %lu reason=%s\n", run, esp_err_to_name(result));
                if (!ramMode) { recording.finish(false); }
            }
        }
    }
    else if (sscanf(line, "ADC127 STOP %lu %c", &run, &extra) == 1 && run == activeRun)
    {
        if (active) { stopCapture("DONE"); }
        else { reply("ADC127 DONE %lu\n", run); }
    }
}

/**
 * @brief Give a batch of decoded numbers to the selected SD or RAM destination.
 * The decoder calls this on Core 1, during decoder.feed(). words points to
 * count 32-bit values: each has 8 status bits and a 24-bit ADC sample. count
 * is a number of words, not bytes. The optional caller argument is unused.
 * The decoder checks status before this call. In SD mode submitSamples()
 * removes status, packs each sample into three bytes, and copies them into
 * the recording ring. True means queued in RAM, not saved to the card.
 * False records an output failure in the decoder. In RAM mode accept()
 * updates the summary immediately and this function returns true, including
 * while skipping startup samples and ignoring samples after the summary fills.
 * Neither destination retains words: the decoder can reuse its batch on return.
 */
static bool recordWords(const uint32_t* words, size_t count, void*)
{
    bool retVal = true;
    if (ramMode) { waveform.accept(words, count); }
    else { retVal = recording.submitSamples(words, count); }
    return retVal;
}

/**
 * @brief Application entry point: set up the P4, then handle commands and faults forever.
 * ESP-IDF calls this once on Core 0 after boot. It has no arguments and does
 * not return. Startup allocates buffers and creates background tasks but does
 * not access the card or start the ADC. Failed required setup aborts through
 * ESP_ERROR_CHECK; the program does not continue with missing resources.
 *
 * The loop serves two separate serial connections. The PC console carries
 * single-character operator commands and printed results. UART1 connects to
 * Feather at 115200 bits/s, receiving on GPIO4 and transmitting on GPIO5;
 * its commands are complete text lines processed by handleLine().
 * A valid Feather ARM2 starts reception. PC keys 't' and 'u' instead run a
 * generated-data SD speed test on Core 1, using a prepared file but no ADC.
 * The loop also checks receive/storage errors and deadlines so capture can
 * stop even if Feather fails to send STOP. Physical ADC stopping still depends
 * on Feather; disabling the P4 receiver only stops this board from collecting.
 */
extern "C" void app_main()
{
    // Allocate the SD ring and start its Core 0 writer task, which waits for
    // a preparation request before touching the card. Also create the Core 1
    // SD-test task; it waits until 't' or 'u'. These allocations happen even
    // if the operator later selects RAM mode. No test starts at this point.
    ESP_ERROR_CHECK(recording.initialize() ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(storageTest.initialize(recording) ? ESP_OK : ESP_ERR_NO_MEM);

    // Tell the decoder which function accepts its completed batches. nullptr
    // means we need no additional caller-supplied context for recordWords().
    decoder.setOutput(recordWords, nullptr);

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
    blocks = static_cast<captureBlock*>(heap_caps_calloc(
        blockCount, sizeof(captureBlock), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    freeBlocks = xQueueCreate(blockCount, sizeof(captureBlock*));
    readyBlocks = xQueueCreate(blockCount + 1, sizeof(captureBlock*));
    drained = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(blocks && freeBlocks && readyBlocks && drained ? ESP_OK : ESP_ERR_NO_MEM);

    // Put each block's address in the free queue once. &blocks[index] selects
    // that block in the array; &block gives the queue the address of the local
    // pointer so it can copy its VALUE. Queue operations do not copy raw data.
    for (unsigned index = 0; index < blockCount; ++index)
    {
        captureBlock* block = &blocks[index];
        xQueueSend(freeBlocks, &block, 0);
    }

    // Initially every block is free. Pin the decoder to CPU1, with a 4096-byte
    // task stack and scheduling priority 10. Stack storage is for function calls
    // and locals; it is separate from the sixteen 4096-byte input blocks.
#if CONFIG_P4_CAPTURE_PROFILE
    TaskHandle_t* decoderHandleOutput = &decoderTaskHandle;
#else
    TaskHandle_t* decoderHandleOutput = nullptr;
#endif
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(decodeTask, "decode", 4096, nullptr, 10,
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
    ESP_ERROR_CHECK(uart_param_config(featherPort, &uartConfig));
    ESP_ERROR_CHECK(uart_set_pin(featherPort, 5, 4, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(featherPort, 2048, 512, 16, &uartEvents, 0));

    // Announce available commands after all required setup has succeeded.
    // The build option chooses optional CPU profiling and test duration;
    // it does not itself begin acquisition or prepare an SD file.
#if CONFIG_P4_CAPTURE_PROFILE
    printf("P4 capture IDLE (sd-profile-60s). Profiling ON; use Feather 60s firmware.\n");
    printf("Press p here and wait for SD READY before starting. "
#else
    printf("P4 capture IDLE (sd-v5-30min). Press p here and wait for SD READY before starting. "
#endif
           "DCLK=0 FSYNC=1 DOUT0=2 RX=4 TX=5.\n");
    printf("ADC files: 3 bytes/sample, status still checked; stage 6 requires 24.576 Mbps to SD.\n");
    printf("For RAM waveform only: press w BEFORE any p/t/u or capture. Mode fixed until reset; no SD needed.\n");
    printf("P4 keys: p=prepare; t=SD-only 26 Mbps; u=SD-only 32.768 Mbps; s=stop SD test. "
           "Each SD test lasts 60 seconds. Do not send Feather commands during SD tests.\n");
    printf("Bench run=%lu seconds; receiver timeout=%lu seconds from ARM.\n",
           static_cast<unsigned long>(ads127BenchTiming::runMs / 1000),
           static_cast<unsigned long>(ads127BenchTiming::receiverTimeoutMs / 1000));
    benchLine line;
    for (;;)
    {
        // Check at most one PC keystroke per pass without waiting for input.
        // Selecting RAM or requesting SD use locks the mode until reset,
        // including an SD request that is subsequently refused.
        char consoleKey = 0;
        if (read(STDIN_FILENO, &consoleKey, 1) == 1)
        {
            if (consoleKey == 'w')
            {
                if (!modeLocked && !active)
                {
                    ramMode = true;
                    modeLocked = true;
                    printf("RAM READY: no card access; use Feather DAC firmware with RAMFULL support, then 6.\n");
                }
                else { printf("Mode already selected; reset P4 before changing modes.\n"); }
            }
            else if (ramMode && (consoleKey == 'p' || consoleKey == 't' || consoleKey == 'u' || consoleKey == 's'))
            {
                printf("RAM mode: SD commands disabled until reset.\n");
            }
            else if (consoleKey == 'p')
            {
                // prepare() requests background work. Its true result means
                // accepted, not ready: the writer later prints SD READY/FAILED.
                modeLocked = true;
                const bool accepted = !active && recording.prepare();
                printf("SD prepare %s\n", accepted ? "requested; wait for SD READY" : "busy/already prepared");
            }
            else if (consoleKey == 't' || consoleKey == 'u')
            {
                // These rates count generated four-byte words per second,
                // giving 26 or 32.768 million bits/s. ADC recordings instead
                // store three bytes/sample. Both tests require a prepared file.
                modeLocked = true;
                const bool accepted = !active && storageTest.start(consoleKey == 't' ? 812500 : 1024000);
                if (!accepted) { printf("SD TEST refused: busy or not prepared; p then SD READY required\n"); }
            }
            else if (consoleKey == 's') { storageTest.stop(); }
        }
        // First collect serial error reports. Timeout 0 means each queue read
        // returns immediately. A bad link clears both the driver's unread
        // characters and our partly assembled line; during capture it is fatal
        // to that run and is included in the reported errors count.
        uart_event_t event = {};
        bool linkError = false;
        while (xQueueReceive(uartEvents, &event, 0) == pdTRUE)
        {
            if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
                event.type == UART_PARITY_ERR || event.type == UART_FRAME_ERR)
            {
                linkError = true;
            }
        }
        if (linkError)
        {
            uart_flush_input(featherPort);
            line = benchLine{};
            if (active)
            {
                callbackErrors.fetch_add(1);
                stopCapture("FAULT");
            }
        }
        // uart_read_bytes() actually copies Feather's incoming characters to
        // this array, up to 256 per pass without waiting. benchLine collects
        // them until newline and rejects oversized lines; feed() returns true
        // only when handleLine() has a complete command to examine.
        uint8_t incoming[256];
        const int received = uart_read_bytes(featherPort, incoming, sizeof(incoming), 0);
        for (int index = 0; index < received; ++index)
        {
            if (line.feed(static_cast<char>(incoming[index]))) { handleLine(line.value()); }
        }
        if (active)
        {
            // A completed RAM window asks the Feather to stop the ADC. Its
            // matching STOP returns through handleLine(), then we dump USB data.
            // Older Feather firmware ignores RAMFULL: bound that wait to 2 s
            // and report FAULT (which also asks older firmware to stop).
            if (ramMode && waveform.complete() && ramStopRequestedAt == 0)
            {
                ramStopRequestedAt = esp_timer_get_time();
                reply("ADC127 RAMFULL %lu\n", activeRun);
            }
            // Stop on lost private blocks, invalid receive chunks, SD failure,
            // or a RAM stop request unanswered for two seconds. Independently,
            // limit total time since ARM to five seconds in RAM mode or the
            // shared test timeout in SD mode. esp_timer_get_time() is in
            // microseconds; multiply millisecond limits by 1000 for comparison.
            if (droppedBlocks.load() != 0 || callbackErrors.load() != 0 ||
                (!ramMode && recording.failed()) ||
                (ramStopRequestedAt != 0 && esp_timer_get_time() - ramStopRequestedAt > 2000000))
            {
                stopCapture("FAULT");
            }
            else if (esp_timer_get_time() - armedAt >=
                     static_cast<int64_t>(ramMode ? 5000 : ads127BenchTiming::receiverTimeoutMs) * 1000)
            {
                stopCapture("TIMEOUT");
            }
#if CONFIG_P4_CAPTURE_PROFILE
            // poll() is normally just a timestamp comparison. About once per
            // second it samples counters without console output or stack scans.
            if (active) { profile.poll(); }
#endif
        }
        // Yield according to the OS tick setting rather than continuously
        // checking commands. This requests a one-millisecond delay converted
        // to ticks; it does not promise a precise one-millisecond loop period.
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
