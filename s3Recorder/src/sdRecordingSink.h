/**
 * @file sdRecordingSink.h
 * @brief Project-specific SD wiring, filesystem and numbered recording files.
 */
#pragma once
#include <bufferedWriter.h>
#include <SPI.h>
#include <SdFat.h>
#include "recordingStatus.h"

/**
 * @brief Adapt ordinary file writes to byteSink without exposing the card to the ADC.
 * Only recordingService's storage task calls this object. SPI3/HSPI uses pins
 * 5/6/7/4; the digitizer keeps its separate SPI2 controller and pins 9-13.
 * SdFs recognizes FAT16, FAT32 and exFAT without formatting. Its sync()/close()
 * return actual errors, unlike Arduino File.flush(), whose return type is void.
 */
class sdRecordingSink : public byteSink
{
public:
    bool mount();
    bool open(uint32_t sampleRate);
    size_t write(const uint8_t* data, size_t length) override;
    bool flush() override;
    bool close(bool complete);
    bool deleteRecordings(uint64_t& deleted);
    void space(uint64_t& total, uint64_t& free);
    uint32_t cardState = recordingStatus::unknown;
    uint32_t session = 0, part = 0;
    uint64_t payloadBytes = 0, headerBytes = 0;
private:
    bool openPart();
    bool closePart(bool complete);
    bool header(uint32_t disposition);
    bool verifyPart(uint32_t disposition);
    static bool recordingName(const char* name, uint32_t& session, uint32_t& part);
    SPIClass bus{HSPI};
    bool mounted = false, busStarted = false;
    SdFs filesystem;
    FsFile file;
    uint32_t rate = 0;
    uint64_t partBytes = 0, partStart = 0;
    uint8_t lastWord[4] = {}; // Last complete payload word, for bounded close-time readback.
    // Split below both FAT32's 4-GiB ceiling and our agreed 1-GiB part limit.
    static constexpr uint64_t partLimit = 1024ULL * 1024 * 1024 - 4096;
};
