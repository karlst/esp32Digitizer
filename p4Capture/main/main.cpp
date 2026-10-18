/**
 * @file main.cpp
 * @brief P4 ADC-to-SD commissioning test, driven by the Feather.
 * Reading path: app_main -> handleLine -> startCapture -> receiveBlock ->
 * decodeTask -> stopCapture; sdRecorder owns asynchronous card operations.
 * No DAC, ADC SPI, Wi-Fi, or automatic start.
 *
 * PARLIO is the parallel-input peripheral. DMA (Direct Memory Access) moves
 * paired DOUT/FSYNC bits from it to a circular memory area without a CPU read
 * per bit. Its ISR (Interrupt Service Routine) copies completed DMA blocks into
 * separately owned buffers. A task (scheduled thread) on CPU1 decodes those
 * buffers. CPU0 handles commands and the shared finite safety deadline.
 *
 * A free-buffer queue transfers ownership to the ISR; a ready-buffer queue
 * transfers it to the decoder. No queued pointer refers to a DMA buffer that
 * hardware can overwrite. A full software queue is a fault, never permission
 * to discard quietly. The IDF cyclic DMA driver does not expose a guaranteed
 * hardware-overrun counter; see README before interpreting zero drops.
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
#include "ads127BenchTiming.h"
#include "benchLine.h"
#include "frameDecoder.h"
#include "sdRecorder.h"
#include "sdWriteTest.h"
#include "sdkconfig.h"

#if !CONFIG_ESP32P4_SELECTS_REV_LESS_V3 || !CONFIG_ESPTOOLPY_FLASHSIZE_16MB
#error "This bench project requires P4 pre-v3 silicon support and 16 MB flash."
#endif

#if !CONFIG_SPIRAM
#error "SD recording needs PSRAM: build with sdkconfig.sd.defaults as documented."
#endif

static constexpr uart_port_t featherPort = UART_NUM_1;
static constexpr size_t dmaBytes = 32768;
static constexpr size_t blockBytes = 4096;
static constexpr unsigned blockCount = 16;
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "ISR counters must not invoke a locking implementation");

/** @brief One private copy, retained until the CPU1 decoder finishes reading it. */
struct captureBlock
{
    size_t length;
    int64_t arrivedAt;
    uint8_t data[blockBytes];
};

/**
 * @brief CPU1 timing totals for one run, read by CPU0 only after draining.
 * Times are microseconds of elapsed wall time, including any preemption.
 * They are diagnostic overhead too, not cycle-exact CPU profiling.
 */
struct decoderTiming
{
    uint64_t decodeUs = 0;
    uint64_t pauseUs = 0;
    uint64_t bytes = 0;
    uint32_t blocks = 0;
    uint32_t pauses = 0;
    int64_t maxDecodeUs = 0;
    int64_t maxWaitUs = 0;
};
static decoderTiming timing;

static captureBlock* blocks = nullptr;
static QueueHandle_t freeBlocks = nullptr;
static QueueHandle_t readyBlocks = nullptr;
static QueueHandle_t uartEvents = nullptr;
static SemaphoreHandle_t drained = nullptr;
static frameDecoder decoder;
static sdRecorder recording;
static sdWriteTest storageTest;
static parlio_rx_unit_handle_t receiver = nullptr;
static parlio_rx_delimiter_handle_t delimiter = nullptr;
static uint8_t* dmaBuffer = nullptr;
static bool enabled = false;
static bool active = false;
static unsigned activeStage = 0;
static unsigned long activeRun = 0;
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
 * @brief CPU0 sends a bounded protocol line to Feather and mirrors it to USB.
 * Messages are emitted only while idle or after stopping DMA. READY means the
 * receiver was armed, not that ADC data have been observed. UART1 GPIO5 is TX;
 * this is the only signal output installed by this application.
 */
static void reply(const char* format, ...)
{
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
 * @brief ISR copies a completed DMA descriptor into one free private block.
 * edata's data pointer belongs to cyclic DMA, so it must not escape this call.
 * memcpy is the actual data transfer into our queue-owned storage. For example,
 * block 3 stays unavailable to this ISR while CPU1 reads it, even after DMA has
 * wrapped around its own 32 KiB area. A failed allocation/queue send is counted.
 * Returning true asks the scheduler to wake a newly runnable higher-priority
 * task. No logging, heap allocation, decoding or waiting occurs in the ISR.
 */
static bool receiveBlock(parlio_rx_unit_handle_t,
                         const parlio_rx_event_data_t* event, void*)
{
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
        block->length = event->recv_bytes;
        block->arrivedAt = now;
        memcpy(block->data, event->data, block->length);
        const int64_t copyUs = esp_timer_get_time() - now;
        if (copyUs > maxCopyUs) { maxCopyUs = copyUs; }
        if (xQueueSendFromISR(readyBlocks, &block, &awakened) != pdTRUE)
        {
            droppedBlocks.fetch_add(1, std::memory_order_relaxed);
            xQueueSendFromISR(freeBlocks, &block, &awakened);
        }
        const uint32_t queued = uxQueueMessagesWaitingFromISR(readyBlocks);
        if (queued > queueHighWater) { queueHighWater = queued; }
    }
    return awakened == pdTRUE;
}

/**
 * @brief CPU1 owns decoder state while running; queue waits let idle CPU sleep.
 * A null pointer is a drain marker inserted after DMA stops. FIFO ordering means
 * all preceding blocks have been processed when the semaphore is given. CPU0
 * can then read/reset decoder state without a race. Normal blocks return to the
 * free queue only AFTER feed reads them and copies decoded words into the SD ring.
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
            const int64_t waitUs = startedAt - block->arrivedAt;
            if (waitUs > timing.maxWaitUs) { timing.maxWaitUs = waitUs; }
            decoder.feed(block->data, block->length);
            const int64_t finishedAt = esp_timer_get_time();
            const int64_t decodeUs = finishedAt - startedAt;
            timing.decodeUs += decodeUs;
            if (decodeUs > timing.maxDecodeUs) { timing.maxDecodeUs = decodeUs; }
            timing.bytes += static_cast<uint32_t>(block->length);
            ++timing.blocks;
            xQueueSend(freeBlocks, &block, portMAX_DELAY);

            // Keep background/idle scheduling opportunities even under backlog,
            // but at most once per 100 ms rather than every sixteen blocks.
            // Reset the deadline on the first block of each run. Normal
            // empty-queue waits also let idle tasks run.
            if (finishedAt - lastPauseAt >= 100000)
            {
                const int64_t pauseStartedAt = esp_timer_get_time();
                vTaskDelay(1);
                lastPauseAt = esp_timer_get_time();
                timing.pauseUs += lastPauseAt - pauseStartedAt;
                ++timing.pauses;
            }
        }
        else
        {
            lastPauseAt = esp_timer_get_time();
            xSemaphoreGive(drained);
        }
    }
}

/**
 * @brief Wait for CPU1 to return every queued block after DMA has been stopped.
 * Used for both normal shutdown and setup failure, so a retry cannot reset
 * decoder state while a previous attempt still owns it. Timeout is a hard fault.
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
 * @brief Stop/release receiver resources, including partially failed setup.
 * This is called on CPU0, the interrupt installation core. Disable before
 * deleting or freeing memory. A driver teardown error is fatal rather than
 * freeing memory that DMA might still use. Feather has its own finite timeout.
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
 * @brief CPU0 arms external-clock capture before granting Feather permission.
 * Stage is already range-checked. Input pins are DCLK0, DOUT0=2, FSYNC1.
 * Two-bit LSB packing stores four input instants in one byte; it does NOT mean
 * ADC words are LSB-first. ADC words still arrive most-significant bit first.
 * The ADC clock can stop between tests, hence free_clk=false. The configured
 * frequency tells the driver the expected input; it does not generate a clock.
 * Return ESP_OK only if receive was queued and capture enabled successfully.
 */
static esp_err_t startCapture(unsigned stage)
{
    const auto& rate = ads127BenchRates[stage];
    decoder.reset(rate.slots * 32U);
    timing = {};
    droppedBlocks.store(0);
    callbackErrors.store(0);
    rawBytes = firstBlockBytes = callbacks = queueHighWater = 0;
    firstBlockAt = lastBlockAt = 0;
    maxCallbackGapUs = maxCopyUs = 0;
    dmaBuffer = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        64, dmaBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    esp_err_t retVal = dmaBuffer != nullptr ? ESP_OK : ESP_ERR_NO_MEM;

    // DMA and private input blocks use internal RAM; only the recording ring
    // uses PSRAM. Both clock and sample lines remain hardware inputs.
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

    // A software delimiter keeps receiving across DMA descriptors. Its large
    // frame limit is not an ADC sample length; on_partial_receive handles each
    // completed descriptor (at most 4092 bytes, with cache-alignment rounding).
    parlio_rx_soft_delimiter_config_t delimiterConfig = {};
    delimiterConfig.sample_edge = PARLIO_SAMPLE_EDGE_POS;
    delimiterConfig.bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB;
    delimiterConfig.eof_data_len = dmaBytes;
    if (retVal == ESP_OK)
    {
        retVal = parlio_new_rx_soft_delimiter(&delimiterConfig, &delimiter);
    }
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
    parlio_receive_config_t receiveConfig = {};
    receiveConfig.delimiter = delimiter;
    receiveConfig.flags.partial_rx_en = true;
    if (retVal == ESP_OK)
    {
        retVal = parlio_rx_unit_receive(receiver, dmaBuffer, dmaBytes, &receiveConfig);
    }
    if (retVal != ESP_OK)
    {
        releaseReceiver();
        drainDecoder();
    }
    return retVal;
}

/**
 * @brief Stop DMA, drain private blocks, then report complete-word observations.
 * kind is DONE for a requested stop, FAULT for software failure, TIMEOUT for
 * the shared receiver deadline without matching STOP. None implies ADC success.
 * The local receiver cannot directly stop ADC conversions: FAULT/TIMEOUT asks
 * Feather to do so. The incomplete hardware descriptor is not delivered on
 * disable, so counts exclude its tail and are not an exact generated total.
 */
static void stopCapture(const char* kind)
{
    releaseReceiver();
    drainDecoder();
    active = false;
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

    // MB/s uses decimal megabytes. decodeMBps measures feed() wall time only;
    // it excludes queue operations and deliberate pauses, so it is an upper
    // bound on whole-worker throughput. Compare with requiredRawMBps, not the
    // smaller useful ADC payload rate. Maximum times expose occasional stalls.
    printf("TIMING decodeMBps=%.3f requiredRawMBps=%.3f decodeAvgUs=%.1f "
           "decodeMaxUs=%lld queueWaitMaxUs=%lld\n",
           timing.decodeUs != 0 ? static_cast<double>(timing.bytes) / timing.decodeUs : 0.0,
           ads127BenchRates[activeStage].samplesPerSecond *
               ads127BenchRates[activeStage].slots * 8.0 / 1000000.0,
           timing.blocks != 0 ? static_cast<double>(timing.decodeUs) / timing.blocks : 0.0,
           static_cast<long long>(timing.maxDecodeUs), static_cast<long long>(timing.maxWaitUs));
    printf("TIMING callbackGapMaxUs=%lld copyMaxUs=%lld pauses=%lu pauseTotalUs=%llu\n",
           static_cast<long long>(maxCallbackGapUs), static_cast<long long>(maxCopyUs),
           static_cast<unsigned long>(timing.pauses), static_cast<unsigned long long>(timing.pauseUs));
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
    recording.finish(strcmp(kind, "DONE") == 0 && stats.words != 0 &&
        stats.badHeaders == 0 && stats.badPeriods == 0 && stats.shortWords == 0 &&
        droppedBlocks.load() == 0 && callbackErrors.load() == 0 && !decoder.outputFailed(), stats.words);
}

/**
 * @brief Parse a complete bounded Feather line; reject trailing junk/run mismatch.
 * ARM2 contains a run ID and rate-table stage. Repeating the active request is
 * idempotent: reply READY2 without resetting the buffers or extending timeout.
 * STOP acknowledges only the matching run, including a run already faulted.
 */
static void handleLine(const char* line)
{
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
        else if (!recording.claim(run, stage, ads127BenchRates[stage].samplesPerSecond))
        {
            reply("ADC127 FAILED %lu reason=SD-not-ready-press-p-on-P4\n", run);
        }
        else
        {
            activeRun = run;
            activeStage = stage;
            const esp_err_t result = startCapture(stage);
            if (result == ESP_OK)
            {
                armedAt = esp_timer_get_time();
                active = true;
                reply("ADC127 READY2 %lu %u\n", run, stage);
            }
            else
            {
                reply("ADC127 FAILED %lu reason=%s\n", run, esp_err_to_name(result));
                recording.finish(false);
            }
        }
    }
    else if (sscanf(line, "ADC127 STOP %lu %c", &run, &extra) == 1 && run == activeRun)
    {
        if (active) { stopCapture("DONE"); }
        else { reply("ADC127 DONE %lu\n", run); }
    }
}

/** @brief CPU1 packs decoded samples to three bytes before the nonblocking ring copy.
 * frameDecoder retains status/header validation and DMA capture is unchanged.
 */
static bool recordWords(const uint32_t* words, size_t count, void*)
{
    return recording.submitSamples(words, count);
}

/**
 * @brief ESP-IDF calls this on CPU0 after boot; initialize once and remain idle.
 * UART1 is the Feather control link, 115200 baud, GPIO4 RX/GPIO5 TX. UART0 on
 * GPIO37/38 remains the USB console. CPU1's worker waits for data; neither task
 * drives DCLK, FSYNC or DOUT0. Only a valid Feather ARM2 starts the receiver.
 * Console t/u instead claim the prepared file for a CPU1 synthetic SD test;
 * they do not configure or start the ADC and never enable the receiver.
 */
extern "C" void app_main()
{
    ESP_ERROR_CHECK(recording.initialize() ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(storageTest.initialize(recording) ? ESP_OK : ESP_ERR_NO_MEM);
    decoder.setOutput(recordWords, nullptr);

    // Nonblocking console reads let the UART control task service Feather even
    // when Karl has not typed anything. Only 'p' prepares a new card file.
    const int consoleFlags = fcntl(STDIN_FILENO, F_GETFL, 0);
    ESP_ERROR_CHECK(consoleFlags >= 0 && fcntl(STDIN_FILENO, F_SETFL,
        consoleFlags | O_NONBLOCK) == 0 ? ESP_OK : ESP_FAIL);

    // Explicit input-only configuration also clears any previous boot pin mode.
    // Disable pulls: the powered translator supplies the actual logic levels.
    gpio_config_t inputs = {};
    inputs.pin_bit_mask = (1ULL << 0) | (1ULL << 1) | (1ULL << 2);
    inputs.mode = GPIO_MODE_INPUT;
    ESP_ERROR_CHECK(gpio_config(&inputs));
    blocks = static_cast<captureBlock*>(heap_caps_calloc(
        blockCount, sizeof(captureBlock), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    freeBlocks = xQueueCreate(blockCount, sizeof(captureBlock*));
    readyBlocks = xQueueCreate(blockCount + 1, sizeof(captureBlock*));
    drained = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(blocks && freeBlocks && readyBlocks && drained ? ESP_OK : ESP_ERR_NO_MEM);
    for (unsigned index = 0; index < blockCount; ++index)
    {
        captureBlock* block = &blocks[index];
        xQueueSend(freeBlocks, &block, 0);
    }
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(decodeTask, "decode", 4096, nullptr, 10,
                                          nullptr, 1) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    // Driver queues buffer short command bursts. Detect framing, parity and
    // overflow events rather than accepting a corrupted control message.
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
    printf("P4 capture IDLE (sd-v5-30min). Press p here and wait for SD READY before starting. "
           "DCLK=0 FSYNC=1 DOUT0=2 RX=4 TX=5.\n");
    printf("ADC files: 3 bytes/sample, status still checked; stage 6 requires 24.576 Mbps to SD.\n");
    printf("P4 keys: p=prepare; t=SD-only 26 Mbps; u=SD-only 32.768 Mbps; s=stop SD test. "
           "Each SD test lasts 60 seconds. Do not send Feather commands during SD tests.\n");
    printf("Bench run=%lu seconds; receiver timeout=%lu seconds from ARM.\n",
           static_cast<unsigned long>(ads127BenchTiming::runMs / 1000),
           static_cast<unsigned long>(ads127BenchTiming::receiverTimeoutMs / 1000));
    benchLine line;
    for (;;)
    {
        char consoleKey = 0;
        if (read(STDIN_FILENO, &consoleKey, 1) == 1)
        {
            if (consoleKey == 'p')
            {
                const bool accepted = !active && recording.prepare();
                printf("SD prepare %s\n", accepted ? "requested; wait for SD READY" : "busy/already prepared");
            }
            else if (consoleKey == 't' || consoleKey == 'u')
            {
                const bool accepted = !active && storageTest.start(consoleKey == 't' ? 812500 : 1024000);
                if (!accepted) { printf("SD TEST refused: busy or not prepared; p then SD READY required\n"); }
            }
            else if (consoleKey == 's') { storageTest.stop(); }
        }
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
        uint8_t incoming[256];
        const int received = uart_read_bytes(featherPort, incoming, sizeof(incoming), 0);
        for (int index = 0; index < received; ++index)
        {
            if (line.feed(static_cast<char>(incoming[index]))) { handleLine(line.value()); }
        }
        if (active)
        {
            if (droppedBlocks.load() != 0 || callbackErrors.load() != 0 || recording.failed())
            {
                stopCapture("FAULT");
            }
            else if (esp_timer_get_time() - armedAt >=
                     static_cast<int64_t>(ads127BenchTiming::receiverTimeoutMs) * 1000)
            {
                stopCapture("TIMEOUT");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
