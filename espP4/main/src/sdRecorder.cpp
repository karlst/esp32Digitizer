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

/** @file SdRecorder.cpp
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
#include "SdRecorder.h"
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
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "File words require little endian");
static_assert(FF_FS_EXFAT == 1 && FF_USE_LFN == 3 && FF_MAX_LFN == 255,
              "P4 requires the project-local exFAT configuration");
static_assert(sizeof(FSIZE_t) == 8, "exFAT file sizes must be 64 bit");

/**
 * @brief Retain the byte reservation and optional verification observer without hardware I/O.
 * Startup selects a smaller reservation for short recordings. The observer is
 * borrowed and must remain alive until the permanent disk task stops (normally reset).
 */
SdRecorder::SdRecorder(uint64_t reservedBytes, RecordingReadback* recordingReadback)
    : _reservedBytes(reservedBytes), _recordingReadback(recordingReadback) {}

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
bool SdRecorder::initialize()
{
    // Retain the task handle for optional observers; this does not enable profiling.
    TaskHandle_t* taskHandle = &_diskTask;
    _scratch = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        64, scratchBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    const bool retVal = _scratch != nullptr && _bufferedWriter.initializeMemory(allocateRing, releaseRing) &&
        xTaskCreatePinnedToCore(taskEntry, "sdWriter", 8192, this, 4, taskHandle, 0) == pdPASS;
    return retVal;
}

/**
 * @brief Borrow the persistent writer task handle for runtime/stack diagnostics.
 * Main calls this after initialize() succeeds. The task is never deleted, so
 * the profiler may inspect it without owning, stopping or scheduling it.
 */
TaskHandle_t SdRecorder::taskHandle() const
{
    return _diskTask;
}

/**
 * @brief Request a new file from Core 0 after idle, completed, or failed recording.
 * True means the request was queued by changing state. The disk task later
 * prints READY or PREPARE FAILED. False means busy or blocked after failed
 * close; neither result changes existing file contents in this function.
 */
bool SdRecorder::prepare()
{
    const phase current = _state.load();
    const bool retVal = current == phase::idle || current == phase::complete || current == phase::error;
    if (retVal) { _state.store(phase::preparing); }
    return retVal;
}

/**
 * @brief Assign an already prepared file to a test before it submits bytes.
 * run identifies the test, stage identifies its format/rate choice, and rate
 * is samples (or generated words) per second for the file header. True changes
 * ready to running; false means no file is ready. No card I/O occurs here.
 */
bool SdRecorder::claim(uint32_t run, uint32_t stage, uint32_t rate, const RecordingLayout* recordingLayout)
{
    const bool retVal = _state.load() == phase::ready;
    if (retVal)
    {
        _recordingLayout = recordingLayout != nullptr ? recordingLayout : &_recordingLayoutDefault;
        _runId = run;
        _stageId = stage;
        _sampleRate = rate;
        _state.store(phase::running);
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
bool SdRecorder::submitSamples(const uint32_t* words, size_t count)
{
    const size_t bytes = recordingFormat::packSamples(words, count, _packedSamples, sizeof(_packedSamples));
    bool retVal = count > 0 && count <= 512 && bytes == count * 3;
    if (retVal) { retVal = submitBytes(_packedSamples, static_cast<uint32_t>(bytes)); }
    else { _storageBroken.store(true); }
    return retVal;
}

/** @brief Producer copies bytes into owned ring memory and checksums accepted bytes.
 * writer.submit is the actual copy; the source batch may be reused afterward.
 * submittedBytes and CRC are read by the disk task only after the producer stops.
 * Here producer means the Core 1 ADC decoder (or mutually exclusive synthetic
 * test). True promises RAM acceptance only. If no space exists, writer.submit()
 * rejects the entire batch and latches failure; it never waits for an SD write.
 */
bool SdRecorder::submitBytes(const uint8_t* data, uint32_t bytes)
{
    const bool retVal = _bufferedWriter.submit(data, bytes);
    if (retVal)
    {
        _submittedCrc = esp_rom_crc32_le(_submittedCrc, data, bytes);
        _submittedBytes += bytes;
    }
    return retVal;
}

/** @brief Atomic writer failure status, safe for the control task during capture. */
bool SdRecorder::failed() const
{
    return _bufferedWriter.failed() || _storageBroken.load();
}

/** @brief Tell the disk task that no more bytes will be submitted for this run.
 * The caller must first stop and finish all decoding or generated-data work.
 * captureClean says whether the source's checks passed; it is not an SD result.
 * ADC calls supply the decoded sample count, so a clean result also requires
 * exactly three accepted bytes per decoded sample. Synthetic calls use zero
 * because their task already checks its expected four-byte word total.
 */
void SdRecorder::finish(bool captureClean, uint32_t expectedSamples)
{
    _clean = captureClean && _recordingLayout->countMatches(_submittedBytes, expectedSamples);
    _state.store(phase::finishing);
}

/**
 * @brief OS task entry: context is the sdRecorder pointer supplied by initialize().
 * Forward to that object's work() loop, which runs for the program's lifetime.
 */
void SdRecorder::taskEntry(void* context)
{
    static_cast<SdRecorder*>(context)->work();
}

/** @brief Service preparation, large writes and final verification without blocking control.
 * One tick of sleep after each pump lets the lower-priority UART control task
 * run on CPU0. A pump transfers at most one 128 KiB ring block.
 */
void SdRecorder::work()
{
    for (;;)
    {
        // Atomic state makes each completed request visible to this task.
        // Only this task opens/writes/closes the file. A failed close leaves
        // blocked state so another request cannot reuse a possibly open handle.
        const phase current = _state.load();
        if (current == phase::preparing)
        {
            const bool prepared = prepareFile();
            bool closed = true;
            if (!prepared && _opened)
            {
                closed = f_close(&_file) == FR_OK;
                _opened = !closed;
            }
            printf("SD %s file=%s (existing files preserved)\n", prepared ? "READY" : "PREPARE FAILED", _path);
            if (!closed) { printf("SD close failed; restart P4 before retrying\n"); }
            _state.store(prepared ? phase::ready : (closed ? phase::error : phase::blocked));
        }
        else if (current == phase::running) { _bufferedWriter.pump(); }
        else if (current == phase::finishing) { finishFile(); }
        vTaskDelay(1);
    }
}

/** @brief Mount without formatting, reserve a new file and initialize its invalid header.
 * Allocation happens before READY because a fragmented card can take seconds.
 * Failure to find enough contiguous space is reported rather than formatting.
 * The board uses LDO4 for the SD GPIO voltage domain and fixed onboard pins.
 */
bool SdRecorder::prepareFile()
{
    bool retVal = true;
    if (_card == nullptr)
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
        if (error == ESP_OK) { error = esp_vfs_fat_sdmmc_mount("/sd", &host, &slot, &mount, &_card); }
        retVal = error == ESP_OK;
        if (!retVal) { printf("SD mount failed: %s; no format attempted\n", esp_err_to_name(error)); }
    }
    if (retVal)
    {
        const BYTE drive = ff_diskio_get_pdrv_card(_card);
        // Random names reduce collisions; CREATE_NEW refuses to overwrite an
        // existing file. Try another name up to 128 times on name collision.
        FRESULT result = FR_EXIST;
        for (unsigned attempt = 0; attempt < 128 && result == FR_EXIST; ++attempt)
        {
            snprintf(_path, sizeof(_path), "%u:/P4%06lx.BIN", unsigned(drive),
                     static_cast<unsigned long>(esp_random() & 0xffffff));
            result = f_open(&_file, _path, FA_CREATE_NEW | FA_READ | FA_WRITE);
        }
        _opened = result == FR_OK;
        if (_opened && _file.obj.fs->fs_type != FS_EXFAT)
        {
            printf("SD preparation requires exFAT; no format attempted\n");
            result = FR_DENIED;
        }
        if (result == FR_OK)
        {
            printf("SD reserving %llu bytes; wait for SD READY\n",
                   static_cast<unsigned long long>(_reservedBytes));
            // Reserve contiguous filesystem space before samples start. The
            // final argument 1 allocates it now. Failure leaves preparation
            // failed; work() attempts to close the newly created file.
            result = f_expand(&_file, _reservedBytes, 1);
            if (result == FR_OK && f_size(&_file) != _reservedBytes) { result = FR_INT_ERR; }
        }
        if (result == FR_OK)
        {
            // Write an invalid initial header and synchronize it to storage.
            // An interrupted capture must not leave a file marked successful.
            recordingFormat::makeHeader(_scratch, false, 0, 0, 0, 0, 0);
            UINT actual = 0;
            result = f_write(&_file, _scratch, 512, &actual);
            if (result == FR_OK && actual != 512) { result = FR_DISK_ERR; }
            if (result == FR_OK) { result = f_sync(&_file); }
        }
        retVal = result == FR_OK;
        if (!retVal) { printf("SD prepare FatFS error=%u\n", unsigned(result)); }
    }
    if (retVal)
    {
        _submittedBytes = _writtenBytes = 0;
        _writeUs = 0;
        _submittedCrc = 0;
        _storageBroken.store(false);
        _clean = false;
        _bufferedWriter.start(*this, recordingClock);
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
size_t SdRecorder::write(const uint8_t* data, size_t length)
{
    const int64_t began = esp_timer_get_time();
    size_t retVal = 0;
    bool ok = _writtenBytes + length + 512 <= _reservedBytes;
    while (retVal < length && ok)
    {
        const size_t remaining = length - retVal;
        const UINT amount = static_cast<UINT>(remaining < scratchBytes ? remaining : scratchBytes);
        memcpy(_scratch, data + retVal, amount);
        UINT actual = 0;
        const FRESULT result = f_write(&_file, _scratch, amount, &actual);
        retVal += actual;
        ok = result == FR_OK && actual == amount;
    }
    _writtenBytes += retVal;
    _writeUs += esp_timer_get_time() - began;
    if (!ok) { _storageBroken.store(true); }
    return retVal;
}

/** @brief Flush file metadata and pending sectors; errors invalidate the recording. */
bool SdRecorder::flush()
{
    const bool synced = f_sync(&_file) == FR_OK;
    return synced && !_storageBroken.load();
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
void SdRecorder::finishFile()
{
    // Yield between final blocks too, so a full ring drain cannot monopolize
    // CPU0. A failed pump stops here; finish still attempts the required flush.
    bool draining = true;
    while (_bufferedWriter.queuedBytes() != 0 && draining)
    {
        draining = _bufferedWriter.pump(true);
        vTaskDelay(1);
    }
    bool ok = _bufferedWriter.finish();
    const auto statistics = _bufferedWriter.snapshot();
    const uint32_t recordBytes = _recordingLayout->recordBytes();
    ok = ok && _clean && _submittedBytes != 0 && _submittedBytes == _writtenBytes &&
         _writtenBytes % recordBytes == 0;
    // Keep only the 512-byte header and the bytes actually written; release
    // the unused end of the reservation. Seek back past the header to
    // read and checksum the payload. Every filesystem result is checked.
    bool ioOk = f_lseek(&_file, 512 + _writtenBytes) == FR_OK;
    if (ioOk) { ioOk = f_truncate(&_file) == FR_OK; }
    if (ioOk) { ioOk = f_sync(&_file) == FR_OK; }
    if (ioOk) { ioOk = f_lseek(&_file, 512) == FR_OK; }
    uint64_t remaining = _writtenBytes;
    uint64_t nextProgress = 256ULL * 1024 * 1024;
    printf("SD verifying %llu bytes; keep card inserted until SD SAVED/FAILED\n",
           static_cast<unsigned long long>(_writtenBytes));
    if (_recordingReadback != nullptr && recordBytes == 3) { _recordingReadback->reset(_sampleRate); }
    uint32_t crc = 0;
    uint64_t patternErrors = 0;
    while (ioOk && remaining != 0)
    {
        const UINT amount = static_cast<UINT>(remaining < scratchBytes ? remaining : scratchBytes);
        UINT actual = 0;
        ioOk = f_read(&_file, _scratch, amount, &actual) == FR_OK && actual == amount;
        if (ioOk)
        {
            crc = esp_rom_crc32_le(crc, _scratch, actual);
            // Observers see only card-read bytes and cannot retain this scratch pointer.
            if (_recordingReadback != nullptr && recordBytes == 3)
            { _recordingReadback->accept(_scratch, actual); }
            patternErrors += _recordingLayout->check(_scratch, actual, _writtenBytes - remaining);
            remaining -= actual;
            if (_writtenBytes - remaining >= nextProgress)
            {
                printf("SD verified %llu / %llu bytes\n",
                       static_cast<unsigned long long>(_writtenBytes - remaining),
                       static_cast<unsigned long long>(_writtenBytes));
                nextProgress += 256ULL * 1024 * 1024;
            }
        }
        vTaskDelay(1);
    }
    // Mark valid only if acquisition, writing, and readback all passed so far.
    // Header synchronization and close must also pass before printing SAVED.
    ok = ok && ioOk && crc == _submittedCrc && patternErrors == 0;
    _recordingLayout->makeHeader(_scratch, ok, _runId, _stageId, _sampleRate, _writtenBytes, _submittedCrc);
    UINT actual = 0;
    const bool headerOk = f_lseek(&_file, 0) == FR_OK &&
        f_write(&_file, _scratch, 512, &actual) == FR_OK && actual == 512 && f_sync(&_file) == FR_OK;
    const bool closeOk = f_close(&_file) == FR_OK;
    _opened = !closeOk;
    ok = ok && headerOk && closeOk;
    printf("SD %s file=%s bytes=%llu crc=%08lx ringPeak=%lu/%lu maxWriteOrFlushUs=%lu "
           "overflow=%lu errors=%lu unsaved=%lu\n", ok ? "SAVED" : "FAILED", _path,
           static_cast<unsigned long long>(_writtenBytes), static_cast<unsigned long>(crc),
           static_cast<unsigned long>(statistics._peak), static_cast<unsigned long>(BufferedWriter::_capacity),
           static_cast<unsigned long>(statistics._maxUs), static_cast<unsigned long>(statistics._overflows),
           static_cast<unsigned long>(statistics._errors), static_cast<unsigned long>(statistics._unsavedBytes));
    if (!closeOk) { printf("SD close failed; restart P4 before retrying\n"); }
    // This is time inside write(), including copying and card waits, excluding
    // deliberate task sleeps, preparation and readback. Sustained success still
    // requires the full timed source and SD SAVED, not this average alone.
    printf("SD writeCallMbps=%.3f\n", _writeUs != 0 ? _writtenBytes * 8.0 / _writeUs : 0.0);
    _recordingLayout->report(patternErrors);
    if (_recordingReadback != nullptr && recordBytes == 3) { _recordingReadback->report(_runId, ok); }
    _state.store(ok ? phase::complete : (closeOk ? phase::error : phase::blocked));
}
