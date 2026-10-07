/**
 * @file sdRecordingSink.h
 * @brief Project-specific SD wiring, filesystem and numbered recording files.
 */
#pragma once
#include <bufferedWriter.h>
#include "sdmmcBlockDevice.h"
#include <SdFat.h>
#include "recordingStatus.h"
#include "recordingConfig.h"

/**
 * @brief Adapt ordinary file writes to byteSink without exposing the card to the ADC.
 * Only recordingService's storage task calls this object. Native four-bit SDMMC
 * uses pins 5/7/6/15/16/4; the digitizer keeps SPI2 and pins 9-13.
 * FsVolume recognizes FAT16, FAT32 and exFAT without formatting. Its sync()/close()
 * return actual errors, unlike Arduino File.flush(), whose return type is void.
 */
class sdRecordingSink : public byteSink
{
public:
    bool mount();
    bool verifyStorage();
    bool open(uint32_t sampleRate);
    size_t write(const uint8_t* data, size_t length) override;
    bool flush() override;
    bool close(bool complete);
    bool deleteRecordings(uint64_t& deleted);
    bool space(uint64_t& total, uint64_t& free);

    // Status values read by the owning disk task when it publishes telemetry.
    // Other threads must use recordingService::snapshot(), not these live fields.
    uint32_t cardState = recordingStatus::unknown;

    // session counts Start-created recordings; part counts files within one
    // recording. Neither number is a sample index or a physical SD sector address.
    uint32_t session = 0, part = 0;

    // Session totals: payload is sample data; headerBytes counts every accepted
    // header write, including rewriting the same 512 bytes during close.
    uint64_t payloadBytes = 0, headerBytes = 0;

    // Sum of rounded file lengths for this session, not bytes repeatedly written.
    // Includes diagnostic preallocation; excludes directory growth. After closing,
    // a fresh free-space scan accounts for released reservation tails.
    uint64_t allocatedBytes = 0;
private:
    bool openPart();
    void accountGrowth();
    bool closePart(bool complete);
    bool header(uint32_t disposition);
    bool verifyPart(uint32_t disposition);
    static bool recordingName(const char* name, uint32_t& session, uint32_t& part);

    // The adapter moves sectors; FsVolume interprets the existing filesystem.
    // Both belong exclusively to the storage task, including their caches.
    sdmmcBlockDevice device;
    bool mounted = false;
    FsVolume filesystem;
    FsFile file;
    uint32_t rate = 0;
    uint64_t partAllocated = 0;

    // Current part payload length, and payload bytes preceding this part in the
    // session. Both exclude headers; partBytes resets on every new part.
    uint64_t partBytes = 0, partStart = 0;
    uint8_t lastWord[4] = {}; // Last complete payload word, for bounded close-time readback.

    // Split below both FAT32's 4-GiB ceiling and our agreed 1-GiB part limit.
    static constexpr uint64_t partLimit = 1024ULL * 1024 * 1024 - 4096;
};
