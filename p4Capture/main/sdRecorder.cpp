/** @file sdRecorder.cpp
 * @brief Four-bit onboard SD recording; no formatting or deletion paths.
 * A 4 MiB PSRAM ring absorbs card stalls, not sustained insufficient throughput.
 * The disk task copies each 16 KiB slice into internal RAM before f_write(),
 * avoiding a dependency on the card driver's ability to DMA from PSRAM.
 */
#include "sdRecorder.h"
#include "sdTestData.h"
#include "recordingFormat.h"
#include "ads127BenchTiming.h"
#include <cstdio>
#include <cstring>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "esp_vfs_fat.h"
#include "diskio_sdmmc.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static constexpr size_t scratchBytes = 16384;
// Total reserved file length, decimal 10 GB including the 512-byte header.
// Allocation occurs before READY, never while the ADC is running.
static constexpr uint64_t reservedBytes = 10000000000ULL;
static_assert(reservedBytes > 512ULL + 1126400ULL * 3 *
              ads127BenchTiming::receiverTimeoutMs / 1000, "Reservation must cover the timeout");
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "File words require little endian");
static_assert(FF_FS_EXFAT == 1 && FF_USE_LFN == 3 && FF_MAX_LFN == 255,
              "P4 requires the project-local exFAT configuration");
static_assert(sizeof(FSIZE_t) == 8, "exFAT file sizes must be 64 bit");

/** @brief Allocate ring storage in external PSRAM; never fall back to internal RAM. */
static uint8_t* allocateRing(uint32_t bytes)
{
    return static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

/**
 * @brief Release the owned ring when the writer destroys its external storage.
 * Match the writer's uint8_t-pointer callback exactly. The call inside this
 * wrapper safely converts that data pointer to the void pointer accepted by
 * ESP-IDF; the two function-pointer types themselves are not interchangeable.
 */
static void releaseRing(uint8_t* memory)
{
    heap_caps_free(memory);
}

/** @brief Supply the generic writer with a microsecond monotonic clock. */
static uint64_t recordingClock()
{
    return static_cast<uint64_t>(esp_timer_get_time());
}

/** @brief Allocate once and start the disk thread without mounting or starting capture. */
bool sdRecorder::initialize()
{
    scratch = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        64, scratchBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    const bool retVal = scratch != nullptr && writer.initializeMemory(allocateRing, releaseRing) &&
        xTaskCreatePinnedToCore(taskEntry, "sdWriter", 8192, this, 4, nullptr, 0) == pdPASS;
    return retVal;
}

/** @brief CPU0 requests preparation; only a closed previous recording may be replaced. */
bool sdRecorder::prepare()
{
    const phase current = state.load();
    const bool retVal = current == phase::idle || current == phase::complete || current == phase::error;
    if (retVal) { state.store(phase::preparing); }
    return retVal;
}

/** @brief Control task claims an already prepared file before arming DMA; no disk wait. */
bool sdRecorder::claim(uint32_t run, uint32_t stage, uint32_t rate)
{
    const bool retVal = state.load() == phase::ready;
    if (retVal)
    {
        runId = run;
        stageId = stage;
        sampleRate = rate;
        state.store(phase::running);
    }
    return retVal;
}

/** @brief CPU1 synthetic test copies four-byte generated words into the ring.
 * This keeps the SD-only 26/32.768 Mbps baselines unchanged. ADC capture calls
 * submitSamples instead. Only one of these producers can own a prepared file.
 */
bool sdRecorder::submitWords(const uint32_t* words, size_t count)
{
    const uint32_t bytes = static_cast<uint32_t>(count * sizeof(uint32_t));
    const auto* data = reinterpret_cast<const uint8_t*>(words);
    return submitBytes(data, bytes);
}

/** @brief Pack at most 512 decoded samples into three-byte records, then copy to ring.
 * No status checks are removed: frameDecoder checks/counts every status before
 * invoking this callback. Invalid status still invalidates the completed run.
 * A failed pack or submission returns false, latching decoder output failure.
 */
bool sdRecorder::submitSamples(const uint32_t* words, size_t count)
{
    const size_t bytes = recordingFormat::packSamples(words, count, packedSamples, sizeof(packedSamples));
    bool retVal = count > 0 && count <= 512 && bytes == count * 3;
    if (retVal) { retVal = submitBytes(packedSamples, static_cast<uint32_t>(bytes)); }
    else { storageBroken.store(true); }
    return retVal;
}

/** @brief Producer copies bytes into owned ring memory and checksums accepted bytes.
 * writer.submit is the actual copy; the source batch may be reused afterward.
 * submittedBytes and CRC are read by the disk task only after the producer stops.
 */
bool sdRecorder::submitBytes(const uint8_t* data, uint32_t bytes)
{
    const bool retVal = writer.submit(data, bytes);
    if (retVal)
    {
        submittedCrc = esp_rom_crc32_le(submittedCrc, data, bytes);
        submittedBytes += bytes;
    }
    return retVal;
}

/** @brief Atomic writer failure status, safe for the control task during capture. */
bool sdRecorder::failed() const
{
    return writer.failed() || storageBroken.load();
}

/** @brief Publish stopped-producer ownership to the disk thread after decoder drain.
 * ADC calls supply the decoded sample count, so a clean result also requires
 * exactly three accepted bytes per decoded sample. Synthetic calls use zero
 * because their task already checks its expected four-byte word total.
 */
void sdRecorder::finish(bool captureClean, uint32_t expectedSamples)
{
    clean = captureClean && (stageId == sdTestData::stage ||
        submittedBytes == static_cast<uint64_t>(expectedSamples) * 3);
    state.store(phase::finishing);
}

/** @brief FreeRTOS entry forwards to the single file-owning thread. */
void sdRecorder::taskEntry(void* context)
{
    static_cast<sdRecorder*>(context)->work();
}

/** @brief Service preparation, large writes and final verification without blocking control.
 * One tick of sleep after each pump lets the lower-priority UART control task
 * run on CPU0. A pump transfers at most one 128 KiB ring block.
 */
void sdRecorder::work()
{
    for (;;)
    {
        const phase current = state.load();
        if (current == phase::preparing)
        {
            const bool prepared = prepareFile();
            bool closed = true;
            if (!prepared && opened)
            {
                closed = f_close(&file) == FR_OK;
                opened = !closed;
            }
            printf("SD %s file=%s (existing files preserved)\n", prepared ? "READY" : "PREPARE FAILED", path);
            if (!closed) { printf("SD close failed; restart P4 before retrying\n"); }
            state.store(prepared ? phase::ready : (closed ? phase::error : phase::blocked));
        }
        else if (current == phase::running) { writer.pump(); }
        else if (current == phase::finishing) { finishFile(); }
        vTaskDelay(1);
    }
}

/** @brief Mount without formatting, reserve a new file and initialize its invalid header.
 * Allocation happens before READY because a fragmented card can take seconds.
 * Failure to find enough contiguous space is reported rather than formatting.
 * The board uses LDO4 for the SD GPIO voltage domain and fixed onboard pins.
 */
bool sdRecorder::prepareFile()
{
    bool retVal = true;
    if (card == nullptr)
    {
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.max_freq_khz = SDMMC_FREQ_DEFAULT;
        sd_pwr_ctrl_ldo_config_t power = {};
        power.ldo_chan_id = 4;
        static sd_pwr_ctrl_handle_t powerHandle = nullptr;
        esp_err_t error = ESP_OK;
        if (powerHandle == nullptr) { error = sd_pwr_ctrl_new_on_chip_ldo(&power, &powerHandle); }
        host.pwr_ctrl_handle = powerHandle;
        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.width = 4;
        slot.clk = GPIO_NUM_43; slot.cmd = GPIO_NUM_44;
        slot.d0 = GPIO_NUM_39; slot.d1 = GPIO_NUM_40;
        slot.d2 = GPIO_NUM_41; slot.d3 = GPIO_NUM_42;
        slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
        esp_vfs_fat_sdmmc_mount_config_t mount = {};
        mount.format_if_mount_failed = false;
        mount.max_files = 2;
        if (error == ESP_OK) { error = esp_vfs_fat_sdmmc_mount("/sd", &host, &slot, &mount, &card); }
        retVal = error == ESP_OK;
        if (!retVal) { printf("SD mount failed: %s; no format attempted\n", esp_err_to_name(error)); }
    }
    if (retVal)
    {
        const BYTE drive = ff_diskio_get_pdrv_card(card);
        FRESULT result = FR_EXIST;
        for (unsigned attempt = 0; attempt < 128 && result == FR_EXIST; ++attempt)
        {
            snprintf(path, sizeof(path), "%u:/P4%06lx.BIN", unsigned(drive),
                     static_cast<unsigned long>(esp_random() & 0xffffff));
            result = f_open(&file, path, FA_CREATE_NEW | FA_READ | FA_WRITE);
        }
        opened = result == FR_OK;
        if (opened && file.obj.fs->fs_type != FS_EXFAT)
        {
            printf("SD preparation requires exFAT for a 10 GB reservation; no format attempted\n");
            result = FR_DENIED;
        }
        if (result == FR_OK)
        {
            printf("SD reserving 10000000000 bytes (10 GB); wait for SD READY\n");
            result = f_expand(&file, reservedBytes, 1);
            if (result == FR_OK && f_size(&file) != reservedBytes) { result = FR_INT_ERR; }
        }
        if (result == FR_OK)
        {
            recordingFormat::makeHeader(scratch, false, false, 0, 0, 0, 0, 0);
            UINT actual = 0;
            result = f_write(&file, scratch, 512, &actual);
            if (result == FR_OK && actual != 512) { result = FR_DISK_ERR; }
            if (result == FR_OK) { result = f_sync(&file); }
        }
        retVal = result == FR_OK;
        if (!retVal) { printf("SD prepare FatFS error=%u\n", unsigned(result)); }
    }
    if (retVal)
    {
        submittedBytes = writtenBytes = 0;
        writeUs = 0;
        submittedCrc = 0;
        storageBroken.store(false);
        clean = false;
        writer.start(*this, recordingClock);
    }
    return retVal;
}

/** @brief Disk-thread byte destination; return the exact prefix accepted by f_write.
 * memcpy copies from the PSRAM ring to internal scratch; f_write performs the
 * card transfer. Ring ownership is retained until every slice returns.
 */
size_t sdRecorder::write(const uint8_t* data, size_t length)
{
    const int64_t began = esp_timer_get_time();
    size_t retVal = 0;
    bool ok = writtenBytes + length + 512 <= reservedBytes;
    while (retVal < length && ok)
    {
        const size_t remaining = length - retVal;
        const UINT amount = static_cast<UINT>(remaining < scratchBytes ? remaining : scratchBytes);
        memcpy(scratch, data + retVal, amount);
        UINT actual = 0;
        const FRESULT result = f_write(&file, scratch, amount, &actual);
        retVal += actual;
        ok = result == FR_OK && actual == amount;
    }
    writtenBytes += retVal;
    writeUs += esp_timer_get_time() - began;
    if (!ok) { storageBroken.store(true); }
    return retVal;
}

/** @brief Flush file metadata and pending sectors; errors invalidate the recording. */
bool sdRecorder::flush()
{
    const bool synced = f_sync(&file) == FR_OK;
    return synced && !storageBroken.load();
}

/** @brief Drain, trim preallocation, verify payload CRC, then write the final header.
 * recordingFormat defines the 512-byte header: ADC v3 has three-byte samples
 * and 64-bit file sizes;
 * the synthetic v1 test retains four-byte words and its original header layout.
 * The valid flag is written last, after readback. A reset during capture leaves
 * valid=0; neither file existence nor ADC DONE alone means a saved recording.
 * Readback uses the mounted card; it is not a power-cycle persistence test.
 */
void sdRecorder::finishFile()
{
    // Yield between final blocks too, so a full ring drain cannot monopolize
    // CPU0. A failed pump stops here; finish still attempts the required flush.
    bool draining = true;
    while (writer.queuedBytes() != 0 && draining)
    {
        draining = writer.pump(true);
        vTaskDelay(1);
    }
    bool ok = writer.finish();
    const auto stats = writer.snapshot();
    const bool synthetic = stageId == sdTestData::stage;
    const uint32_t recordBytes = synthetic ? 4 : 3;
    ok = ok && clean && submittedBytes != 0 && submittedBytes == writtenBytes &&
         writtenBytes % recordBytes == 0;
    bool ioOk = f_lseek(&file, 512 + writtenBytes) == FR_OK;
    if (ioOk) { ioOk = f_truncate(&file) == FR_OK; }
    if (ioOk) { ioOk = f_sync(&file) == FR_OK; }
    if (ioOk) { ioOk = f_lseek(&file, 512) == FR_OK; }
    uint64_t remaining = writtenBytes;
    uint64_t nextProgress = 256ULL * 1024 * 1024;
    printf("SD verifying %llu bytes; keep card inserted until SD SAVED/FAILED\n",
           static_cast<unsigned long long>(writtenBytes));
    uint32_t crc = 0;
    uint64_t patternErrors = 0;
    while (ioOk && remaining != 0)
    {
        const UINT amount = static_cast<UINT>(remaining < scratchBytes ? remaining : scratchBytes);
        UINT actual = 0;
        ioOk = f_read(&file, scratch, amount, &actual) == FR_OK && actual == amount;
        if (ioOk)
        {
            crc = esp_rom_crc32_le(crc, scratch, actual);
            // Synthetic files have a known word at every position. Check bytes
            // read from the card, not a second copy of the submitted RAM buffer.
            if (stageId == sdTestData::stage)
            {
                const uint64_t firstWord = (writtenBytes - remaining) / 4;
                for (UINT offset = 0; offset + 4 <= actual; offset += 4)
                {
                    uint32_t observed = 0;
                    memcpy(&observed, scratch + offset, sizeof(observed));
                    if (observed != sdTestData::word(static_cast<uint32_t>(firstWord + offset / 4)))
                    {
                        ++patternErrors;
                    }
                }
                if (actual % 4 != 0) { ++patternErrors; }
            }
            remaining -= actual;
            if (writtenBytes - remaining >= nextProgress)
            {
                printf("SD verified %llu / %llu bytes\n",
                       static_cast<unsigned long long>(writtenBytes - remaining),
                       static_cast<unsigned long long>(writtenBytes));
                nextProgress += 256ULL * 1024 * 1024;
            }
        }
        vTaskDelay(1);
    }
    ok = ok && ioOk && crc == submittedCrc && patternErrors == 0;
    recordingFormat::makeHeader(scratch, synthetic, ok, runId, stageId, sampleRate,
                               writtenBytes, submittedCrc);
    UINT actual = 0;
    const bool headerOk = f_lseek(&file, 0) == FR_OK &&
        f_write(&file, scratch, 512, &actual) == FR_OK && actual == 512 && f_sync(&file) == FR_OK;
    const bool closeOk = f_close(&file) == FR_OK;
    opened = !closeOk;
    ok = ok && headerOk && closeOk;
    printf("SD %s file=%s bytes=%llu crc=%08lx ringPeak=%lu/%lu maxWriteOrFlushUs=%lu "
           "overflow=%lu errors=%lu unsaved=%lu\n", ok ? "SAVED" : "FAILED", path,
           static_cast<unsigned long long>(writtenBytes), static_cast<unsigned long>(crc),
           static_cast<unsigned long>(stats.peak), static_cast<unsigned long>(bufferedWriter::capacity),
           static_cast<unsigned long>(stats.maxUs), static_cast<unsigned long>(stats.overflows),
           static_cast<unsigned long>(stats.errors), static_cast<unsigned long>(stats.unsavedBytes));
    if (!closeOk) { printf("SD close failed; restart P4 before retrying\n"); }
    // This is time inside write(), including copying and card waits, excluding
    // deliberate task sleeps, preparation and readback. Sustained success still
    // requires the full timed source and SD SAVED, not this average alone.
    printf("SD writeCallMbps=%.3f\n", writeUs != 0 ? writtenBytes * 8.0 / writeUs : 0.0);
    if (stageId == sdTestData::stage)
    {
        printf("SD TEST patternErrors=%llu\n", static_cast<unsigned long long>(patternErrors));
    }
    state.store(ok ? phase::complete : (closeOk ? phase::error : phase::blocked));
}
