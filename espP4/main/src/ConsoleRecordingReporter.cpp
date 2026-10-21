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

/** @file ConsoleRecordingReporter.cpp
 * @brief Format terminal diagnostics from completed recording snapshots.
 */
#include "ConsoleRecordingReporter.h"
#include "AdcReceiver.h"
#include <cstdio>
#include "esp_err.h"

/** @brief Explain the current console controller's startup actions; no recording has started. */
void ConsoleRecordingReporter::startup() const
{
    message("P4 receiver ready for commands. Start from Feather; storage prepares automatically. DCLK=0 FSYNC=1 DOUT0=2 RX=4 TX=5.\n");
    message("ADC files: 3 bytes/sample, status still checked; 1,024,000 samples/s requires 24.576 Mbps to SD.\n");
}

/** @brief Echo a complete controller message to the PC without interpreting percent signs. */
void ConsoleRecordingReporter::message(const char* text) const
{ if (_enabled) { printf("%s", text); } }

/** @brief Print frozen receive/decode details; these do not certify final SD contents. */
void ConsoleRecordingReporter::capture(const RecordingResult& recordingResult) const
{
    if (_enabled)
    {
        const auto& captureStats = recordingResult._captureStats;
        const auto& receiverStats = recordingResult._receiverStats;
        printf("requested=%lu/s bytes=%llu blocks=%lu queueHigh=%lu/%u "
               "tailBits=%lu raw=%08lx sample=%ld\n", static_cast<unsigned long>(recordingResult._sampleRate),
               static_cast<unsigned long long>(receiverStats._rawBytes),
               static_cast<unsigned long>(receiverStats._callbacks),
               static_cast<unsigned long>(receiverStats._queueHighWater), AdcReceiver::getBlockCount(),
               static_cast<unsigned long>(recordingResult._pendingBits),
               static_cast<unsigned long>(captureStats._latestWord), static_cast<long>(captureStats._latestSample));

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
    
    }
}

/** @brief The SD task reports actual preparation completion, not just request acceptance. */
void ConsoleRecordingReporter::storagePrepared(bool ready, const char* path) const
{ if (_enabled) { printf("SD %s file=%s (existing files preserved)\n", ready ? "READY" : "PREPARE FAILED", path); } }

/** @brief Report a storage failure without changing retry or recording policy. */
void ConsoleRecordingReporter::storageError(StorageError error, int code) const
{
    if (_enabled)
    {
        switch (error)
        {
        case StorageError::close:
            message("SD close failed; restart P4 before retrying\n");
            break;
        case StorageError::mount:
            printf("SD mount failed: %s; no format attempted\n", esp_err_to_name(code));
            break;
        case StorageError::needsExfat:
            message("SD preparation requires exFAT; no format attempted\n");
            break;
        case StorageError::prepare:
            printf("SD prepare FatFS error=%u\n", unsigned(code));
            break;
        }
    }
}

/** @brief Report the requested allocation size; READY confirms later success. */
void ConsoleRecordingReporter::storageReserved(uint64_t bytes) const
{ if (_enabled) { printf("SD reserving %llu bytes; wait for SD READY\n", static_cast<unsigned long long>(bytes)); } }

/** @brief Announce readback of the recorded payload after pending writes finish. */
void ConsoleRecordingReporter::storageVerifying(uint64_t bytes) const
{ if (_enabled) { printf("SD verifying %llu bytes; keep card inserted until SD SAVED/FAILED\n", static_cast<unsigned long long>(bytes)); } }

/** @brief Report how many bytes have actually been read back, not merely queued. */
void ConsoleRecordingReporter::storageProgress(uint64_t bytes, uint64_t total) const
{ if (_enabled) { printf("SD verified %llu / %llu bytes\n", static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(total)); } }

/**
 * @brief Report final file status and frozen ring statistics after close was attempted.
 * The recorder supplies success only after acquisition, writing and readback
 * checks. Write Mbps counts time inside write calls, not end-to-end throughput.
 */
void ConsoleRecordingReporter::storageFinished(bool ok, const char* path, uint64_t bytes, uint32_t crc,
    const BufferedWriter::Statistics& statistics, double writeMbps) const
{
    if (_enabled)
    {
        printf("SD %s file=%s bytes=%llu crc=%08lx ringPeak=%lu/%lu maxWriteOrFlushUs=%lu "
               "overflow=%lu errors=%lu unsaved=%lu\n", ok ? "SAVED" : "FAILED", path,
               static_cast<unsigned long long>(bytes), static_cast<unsigned long>(crc),
               static_cast<unsigned long>(statistics._peak), static_cast<unsigned long>(BufferedWriter::_capacity),
               static_cast<unsigned long>(statistics._maxUs), static_cast<unsigned long>(statistics._overflows),
               static_cast<unsigned long>(statistics._errors), static_cast<unsigned long>(statistics._unsavedBytes));
        printf("SD writeCallMbps=%.3f\n", writeMbps);
    }
}
