/**
 * @file sdWriteTiming.cpp
 * @brief Isolated fixed-duration benchmark of repeated fixed-size file writes.
 * Read setup(), loop(), then runTest(). USB command R starts one run; boot alone
 * does not write a file. There is no digitizer, ring buffer, or Feather control.
 * Arduino's loop task owns the card and file, so no shared-data lock is needed.
 * Each write appends the SAME data to a new file on the existing filesystem.
 * Existing files are never overwritten, deleted, or reformatted.
 * Times cover file.write(), including filesystem allocation and card waiting,
 * not just electrical transfers. Statistics use every successful full write;
 * failures have their duration reported separately. Opening and final sync are
 * outside the configured writing window. No free-space scan or preallocation.
 */
#if S3_SD_WRITE_TIMING
#include <Arduino.h>
#include <esp_timer.h>
#include <cmath>
#include <algorithm>
#include "sdmmcBlockDevice.h"

// Defaults reproduce the original 4-KiB/two-minute test. The 8/16/32/64-KiB build
// environments set both this size and the adapter transfer limit, for 60 seconds.
#ifndef S3_WRITE_BLOCK_BYTES
#define S3_WRITE_BLOCK_BYTES 4096
#endif
#ifndef S3_WRITE_SECONDS
#define S3_WRITE_SECONDS 120
#endif
static_assert(S3_WRITE_BLOCK_BYTES == S3_SD_TRANSFER_BYTES,
    "Timing test file block and SD transfer limit must match");

// Static internal RAM keeps the hardware transfer buffer alive for every call.
static sdmmcBlockDevice device;
static FsVolume volume;
static bool mounted = false;
static uint8_t block[S3_WRITE_BLOCK_BYTES];

/**
 * @brief Append fixed blocks for the configured duration and print statistics.
 * Called synchronously by loop() on R. The last write may finish after the
 * deadline; elapsed time includes it. No intentional delay separates writes.
 * Welford's running mean/squared deviations avoid storing thousands of timings.
 * Population standard deviation describes ALL completed writes in this run,
 * rather than estimating a larger population from a sample. A failed or partial
 * write stops the run immediately; its bytes are counted but excluded from the
 * full-block timing distribution. Flush success does not prove power-loss safety
 * or full payload readback. The retained file is available for later inspection.
 */
static void runTest()
{
    if (!mounted)
    {
        Serial.println("TIMING ERROR: card not mounted; reboot to retry initialization.");
        return;
    }

    // Exclusive create prevents replacing an earlier test or an unrelated file.
    char name[48];
    uint32_t id = 1;
    do
    {
        snprintf(name, sizeof(name), "/write_timing_%06lu.bin", static_cast<unsigned long>(id++));
    } while (volume.exists(name) && id < 1000000);
    FsFile file;
    const int64_t openStart = esp_timer_get_time();
    const bool opened = file.open(&volume, name, O_WRONLY | O_CREAT | O_EXCL);
    const int64_t openUs = esp_timer_get_time() - openStart;
    if (!opened)
    {
        Serial.println("TIMING ERROR: exclusive file creation failed.");
        return;
    }
    Serial.printf("TIMING BEGIN file=%s block=%u transfer_limit=%u duration_s=%u open_us=%llu\n",
        name, static_cast<unsigned>(sizeof(block)), static_cast<unsigned>(S3_SD_TRANSFER_BYTES),
        static_cast<unsigned>(S3_WRITE_SECONDS), static_cast<unsigned long long>(openUs));
    Serial.flush(); // Finish the announcement before the throughput clock starts.

    uint32_t count = 0, minUs = UINT32_MAX, maxUs = 0;
    uint64_t bytes = 0;
    double meanUs = 0, squaredDeviations = 0;
    bool ok = true;
    const int64_t began = esp_timer_get_time();
    int64_t nextReport = began + 10000000;
    while (ok && esp_timer_get_time() - began < static_cast<int64_t>(S3_WRITE_SECONDS) * 1000000)
    {
        // This is the actual file write. The same block bytes are reused only
        // after it returns; the SD adapter waits for its hardware transfer.
        const int64_t writeStart = esp_timer_get_time();
        const size_t written = file.write(block, sizeof(block));
        const uint32_t durationUs = esp_timer_get_time() - writeStart;
        bytes += written;
        ok = written == sizeof(block);
        if (ok)
        {
            ++count;
            minUs = std::min(minUs, durationUs);
            maxUs = std::max(maxUs, durationUs);
            const double delta = durationUs - meanUs;
            meanUs += delta / count;
            squaredDeviations += delta * (durationUs - meanUs);
        }
        else
        {
            Serial.printf("TIMING WRITE ERROR bytes=%u expected=%u duration_us=%lu\n",
                static_cast<unsigned>(written), static_cast<unsigned>(sizeof(block)),
                static_cast<unsigned long>(durationUs));
        }

        // Sparse progress is outside individual write timings but INCLUDED in
        // total elapsed time. No per-write serial logging distorts the test.
        const int64_t now = esp_timer_get_time();
        if (now >= nextReport)
        {
            Serial.printf("TIMING progress elapsed_s=%.1f writes=%lu max_us=%lu\n",
                (now - began) / 1000000.0, static_cast<unsigned long>(count),
                static_cast<unsigned long>(maxUs));
            nextReport = now + 10000000;
        }
    }
    const int64_t elapsedUs = esp_timer_get_time() - began;

    // Sync commits filesystem caches separately; report failure even if every
    // earlier write succeeded. File size is bookkeeping, not payload validation.
    const int64_t syncStart = esp_timer_get_time();
    const bool synced = file.sync();
    const int64_t syncUs = esp_timer_get_time() - syncStart;
    const uint64_t fileBytes = file.fileSize();
    file.close();
    Serial.printf("TIMING RESULT ok=%u writes=%lu bytes=%llu elapsed_us=%llu min_us=%lu max_us=%lu mean_us=%.3f stddev_us=%.3f Mbps=%.6f sync_ok=%u sync_us=%llu file_bytes=%llu\n",
        ok && synced && fileBytes == bytes, static_cast<unsigned long>(count),
        static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(elapsedUs),
        static_cast<unsigned long>(count ? minUs : 0), static_cast<unsigned long>(maxUs),
        meanUs, count ? std::sqrt(squaredDeviations / count) : 0.0,
        bytes * 8.0 / elapsedUs, synced, static_cast<unsigned long long>(syncUs),
        static_cast<unsigned long long>(fileBytes));
    Serial.println("TIMING DONE; file retained; R starts another run.");
}

/**
 * @brief Arduino startup: initialize USB, repeatable data, and existing exFAT/FAT.
 * The shared SD adapter applies and verifies the 10 MHz/four-wire configuration.
 * The generated block contains changing byte values but remains identical across
 * writes. Mounting reads filesystem metadata; it never formats the card.
 */
void setup()
{
    Serial.setTxBufferSize(1024);
    Serial.begin(115200);
    for (size_t index = 0; index < sizeof(block); ++index)
    {
        block[index] = static_cast<uint8_t>((index * 37) ^ (index >> 3));
    }
    if (device.begin())
    {
        mounted = volume.begin(&device);
        if (!mounted)
        {
            mounted = volume.begin(&device, true, 0);
        }
    }
    Serial.printf("TIMING READY mounted=%u filesystem=%u; send R for %us of %u-byte writes.\n",
        mounted, mounted ? volume.fatType() : 0, static_cast<unsigned>(S3_WRITE_SECONDS),
        static_cast<unsigned>(sizeof(block)));
}

/**
 * @brief Arduino polls USB for explicit R; sleeps only while the test is idle.
 * The benchmark runs to its deadline or a write failure, then returns here.
 */
void loop()
{
    if (Serial.available() && Serial.read() == 'R')
    {
        runTest();
    }
    delay(1);
}
#endif
