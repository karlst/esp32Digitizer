/**
 * @file sdmmcBlockDevice.h
 * @brief Connect SdFat's FAT/exFAT filesystem to the S3 native SD controller.
 */
#pragma once
#include <SdFat.h>
#include "recordingConfig.h"
#if S3_STORAGE_TYPES_ONLY

/** @brief Desktop command tests replace storage; this type has no hardware behavior. */
class sdmmcBlockDevice {};
#else
#include <sdmmc_cmd.h>

// Normal recordings retain 4-KiB transfers. Diagnostic builds can override this
// byte limit to match their file-write size; all translation units use one flag.
#ifndef S3_SD_TRANSFER_BYTES
#define S3_SD_TRANSFER_BYTES 4096
#endif
static_assert(S3_SD_TRANSFER_BYTES >= 512 && S3_SD_TRANSFER_BYTES % 512 == 0,
    "SD transfer buffer must hold a whole number of 512-byte sectors");

/**
 * @brief Transfer 512-byte card sectors using four data wires and hardware DMA.
 * Only the recordingService disk task calls this class. SdFat requests sectors
 * while managing files; this object knows nothing about filenames or samples.
 * begin() initializes the card but never formats it. Read begin(), transfer(),
 * then syncDevice() to follow setup, data movement and completion checking.
 */
class sdmmcBlockDevice : public FsBlockDeviceInterface
{
public:
    bool begin();
    void end() override;
    bool isBusy() override;
    bool readSector(Sector_t sector, uint8_t* destination) override;
    bool readSectors(Sector_t sector, uint8_t* destination, size_t count) override;
    bool writeSector(Sector_t sector, const uint8_t* source) override;
    bool writeSectors(Sector_t sector, const uint8_t* source, size_t count) override;
    Sector_t sectorCount() override;
    bool syncDevice() override;
private:
    bool transfer(Sector_t sector, uint8_t* destination, const uint8_t* source, size_t count);
    sdmmc_card_t card = {};
    bool hostStarted = false, ready = false;

    // This object lives in internal RAM inside the application's acquisition
    // object. Four-byte alignment and a dedicated buffer satisfy the controller's
    // DMA requirements even when SdFat supplies an unaligned cache pointer.
    alignas(4) uint8_t transferBuffer[S3_SD_TRANSFER_BYTES] = {};
};

#endif
