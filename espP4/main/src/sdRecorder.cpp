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

/** @file sdRecorder.cpp
 * @brief Save received sample bytes to a new file on the P4's onboard SD card.
 * initialize() allocates memory once. prepare() asks the disk task to open and
 * reserve a file; claim() assigns that prepared file to a run. Core 1 then
 * submits data while the Core 0 disk task writes it. finish() requests final
 * writing and verification after submissions stop. No method formats the card
 * or deletes existing files. Four-bit SD means four parallel data wires.
 * A 4 MiB PSRAM ring absorbs card stalls, not sustained insufficient throughput.
 * The disk task copies each 16 KiB slice into internal RAM before f_write(),
 * avoiding a dependency on the card driver's ability to DMA from PSRAM.
 *
 * Reading path: submitSamples() -> submitBytes() for Core 1's data submission;
 * work() -> bufferedWriter::pump() -> write() for Core 0's actual card writes;
 * finish() -> finishFile() for shutdown and verification. A task is a software
 * thread scheduled by FreeRTOS, not an interrupt handler. Both paths can run
 * concurrently on different cores, coordinated by the writer's atomic head/tail
 * positions (positions updated so the other core sees complete copies).
 *
 * Core 1 receives decoded 32-bit status+sample words, packs their low 24 bits
 * into packedSamples, then writer.submit() COPIES those bytes to ring memory.
 * No queued pointer refers back to the decoder batch or its raw input block.
 * Core 0 later asks writer.pump() to write a contiguous span, normally up to
 * 128 KiB. This calls our write(), which uses a 16 KiB temporary internal-RAM
 * buffer and f_write(). The ring span remains unavailable for reuse until the
 * write succeeds. PSRAM is external RAM, separate from the internal DMA and
 * raw-block allocations in main.cpp. The 4 MiB ring holds about 1.37 seconds of
 * stage-6 three-byte samples when empty; it cannot fix a persistently slow card.
 *
 * SD READY means the file was prepared, ADC DONE means capture stopped, and
 * SD SAVED means capture checks plus drain/readback/header/close succeeded.
 * CRC is a numerical checksum calculated from the byte sequence. Full readback
 * compares the saved-data checksum with the accepted-data checksum; it does not prove
 * analog accuracy or that hardware delivered every ADC-generated sample.
 */
#include "sdRecorder.h"
#include "sdTestData.h"
#include "recordingFormat.h"
#include "test/support/ads127BenchTiming.h"
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

/**
 * @brief At boot, allocate the ring and write workspace and create the disk task.
 * app_main() calls this once. True means allocations/task creation succeeded,
 * not that a card is present. The 64-byte alignment is for the storage driver;
 * the new task runs on Core 0 with an 8192-byte stack and priority 4.
 * A false return is fatal to startup in main.cpp; this is not a retry interface.
 */
bool sdRecorder::initialize()
{
    // Retain the task handle only in diagnostic builds. The normal build does
    // not allocate profiling state or ask FreeRTOS to return this handle.
#if CONFIG_P4_CAPTURE_PROFILE
    TaskHandle_t* taskHandle = &diskTask;
#else
    TaskHandle_t* taskHandle = nullptr;
#endif
    scratch = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        64, scratchBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    const bool retVal = scratch != nullptr && writer.initializeMemory(allocateRing, releaseRing) &&
        xTaskCreatePinnedToCore(taskEntry, "sdWriter", 8192, this, 4, taskHandle, 0) == pdPASS;
    return retVal;
}

#if CONFIG_P4_CAPTURE_PROFILE
/**
 * @brief Borrow the persistent writer task handle for runtime/stack diagnostics.
 * Main calls this after initialize() succeeds. The task is never deleted, so
 * the profiler may inspect it without owning, stopping or scheduling it.
 */
TaskHandle_t sdRecorder::profileTask() const
{
    return diskTask;
}
#endif

/**
 * @brief Request a new file from Core 0 after idle, completed, or failed recording.
 * True means the request was queued by changing state. The disk task later
 * prints READY or PREPARE FAILED. False means busy or blocked after failed
 * close; neither result changes existing file contents in this function.
 */
bool sdRecorder::prepare()
{
    const phase current = state.load();
    const bool retVal = current == phase::idle || current == phase::complete || current == phase::error;
    if (retVal) { state.store(phase::preparing); }
    return retVal;
}

/**
 * @brief Assign an already prepared file to a test before it submits bytes.
 * run identifies the test, stage identifies its format/rate choice, and rate
 * is samples (or generated words) per second for the file header. True changes
 * ready to running; false means no file is ready. No card I/O occurs here.
 */
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

/** @brief Pack at most 512 decoded samples into three-byte records, then copy to ring.
 * No status checks are removed: frameDecoder checks/counts every status before
 * invoking this callback. Invalid status still invalidates the completed run.
 * A failed pack or submission returns false, latching decoder output failure.
 * Called on Core 1 by recordWords(), not by an ISR. count is WORDS, while bytes
 * and ring capacity are BYTES. 0x08123456 becomes 56 34 12 in the file: discard
 * status 08 after checking it, store the least-significant sample byte first.
 * A bad header's sample is still submitted; final captureClean will be false.
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
 * Here producer means the Core 1 ADC decoder (or mutually exclusive synthetic
 * test). True promises RAM acceptance only. If no space exists, writer.submit()
 * rejects the entire batch and latches failure; it never waits for an SD write.
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

/** @brief Tell the disk task that no more bytes will be submitted for this run.
 * The caller must first stop and finish all decoding or generated-data work.
 * captureClean says whether the source's checks passed; it is not an SD result.
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

/**
 * @brief OS task entry: context is the sdRecorder pointer supplied by initialize().
 * Forward to that object's work() loop, which runs for the program's lifetime.
 */
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
        // Atomic state makes each completed request visible to this task.
        // Only this task opens/writes/closes the file. A failed close leaves
        // blocked state so another request cannot reuse a possibly open handle.
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
        // Mount means make the card's existing filesystem accessible. LDO4
        // controls the board's SD power/voltage connection. Use the board's
        // six fixed SD pins and four data lines; never format on mount failure.
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
        // Random names reduce collisions; CREATE_NEW refuses to overwrite an
        // existing file. Try another name up to 128 times on name collision.
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
            // Reserve contiguous filesystem space before samples start. The
            // final argument 1 allocates it now. Failure leaves preparation
            // failed; work() attempts to close the newly created file.
            result = f_expand(&file, reservedBytes, 1);
            if (result == FR_OK && f_size(&file) != reservedBytes) { result = FR_INT_ERR; }
        }
        if (result == FR_OK)
        {
            // Write an invalid initial header and synchronize it to storage.
            // An interrupted capture must not leave a file marked successful.
            recordingFormat::makeHeader(scratch, false, 0, 0, 0, 0, 0);
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
 * bufferedWriter::pump() calls this on Core 0 with a pointer into its ring.
 * data+retVal advances past bytes already written; amount limits each copy to
 * scratch capacity. f_write() is a FatFs filesystem call, which reaches the
 * SDMMC driver and SD hardware; it may wait for the card. It is not PARLIO DMA.
 * DMA/SD interrupt handling belongs to those drivers, not this ordinary task.
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

/** @brief Finish queued writes, shorten the reserved file, and verify its contents.
 * Only work() calls this, after the source has stopped submitting. Readback
 * checks every written payload byte through its checksum; generated-data
 * tests also compare every word with its expected pattern.
 * recordingFormat defines the 512-byte header: ADC v3 has three-byte samples
 * and 64-bit file sizes;
 * the synthetic v1 test retains four-byte words and its original header layout.
 * The valid flag is written after readback, before the final sync and close.
 * A close failure can therefore report FAILED even if that flag reached disk.
 * A reset during capture leaves
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
    // Keep only the 512-byte header and the bytes actually written; release
    // the unused end of the 10 GB reservation. Seek back past the header to
    // read and checksum the payload. Every filesystem result is checked.
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
                patternErrors += sdTestData::checkBlock(scratch, actual, (writtenBytes - remaining) / 4);
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
    // Mark valid only if acquisition, writing, and readback all passed so far.
    // Header synchronization and close must also pass before printing SAVED.
    ok = ok && ioOk && crc == submittedCrc && patternErrors == 0;
    if (synthetic)
    {
        sdTestData::makeHeader(scratch, ok, runId, stageId, sampleRate, writtenBytes, submittedCrc);
    }
    else
    {
        recordingFormat::makeHeader(scratch, ok, runId, stageId, sampleRate, writtenBytes, submittedCrc);
    }
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
