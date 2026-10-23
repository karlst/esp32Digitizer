// Copyright (C) 2026 Karl Stock
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file sessionChecks.cpp
 * Exercise real header/backend/buffer code with RAM FatFs, never an SD card.
 * Small file and ring sizes expose boundaries quickly. The hardware test remains
 * necessary for real allocation, task timing, write latency and retained readback.
 */
#include "FileRecordingBackend.h"
#include "MemoryRecordingDisk.h"
#include "RetainedPayloadChecksums.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <stdexcept>

/* **
 * require - Abort this test run with an actionable assertion instead of silent
 * failure.
 */
static void require(bool condition, const char* description)
{
    if (!condition)
    {
        throw std::runtime_error(description);
    }
}

/* **
 * allocateRing - Allocate the real writer's small ring in ordinary PC memory.
 */
static uint8_t* allocateRing(uint32_t bytes)
{
    return static_cast<uint8_t*>(malloc(bytes));
}

/* **
 * releaseRing - Match the allocation callback; called by BufferedWriter's
 * destructor.
 */
static void releaseRing(uint8_t* data)
{
    free(data);
}

/* **
 * clockUs - Deterministic increasing timer for latency/statistics code; not a
 * benchmark.
 */
static uint64_t clockUs()
{
    static uint64_t ticks = 0;
    return ++ticks;
}

/* **
 * utcSeconds - Fixed valid calendar time: 2024-01-01 00:00:00 UTC.
 */
static uint64_t utcSeconds()
{
    return 1704067200ULL;
}

/* **
 * checkPayload - Expected stored stream wraps its byte value every 256 bytes.
 */
static bool checkPayload(const uint8_t* data, uint32_t length, uint64_t offset)
{
    bool retVal = true;
    for (uint32_t iByte = 0; retVal && iByte < length; ++iByte)
    {
        retVal = data[iByte] == static_cast<uint8_t>(offset + iByte);
    }
    return retVal;
}

/* **
 * offer - Offer one bounded batch and optionally drain while full write blocks
 * are available.
 */
static uint32_t offer(FileRecordingBackend& fileRecordingBackend, uint32_t length, uint64_t offset, bool drain = true)
{
    std::vector<uint8_t> data(length);
    for (uint32_t iByte = 0; iByte < length; ++iByte)
    {
        data[iByte] = static_cast<uint8_t>(offset + iByte);
    }
    const uint32_t retVal = fileRecordingBackend.copyToRingBuffer(data.data(), length);
    while (drain && fileRecordingBackend.getQueuedBytes() >= BUFFERED_WRITER_BLOCK_BYTES)
    {
        require(fileRecordingBackend.pump(), "pump failed");
    }
    return retVal;
}

/* **
 * header - Read and independently decode the persisted header for assertions.
 */
static SessionFileHeader header(const char* path)
{
    SessionFileHeader retVal;
    const auto& bytes = MemoryRecordingDisk::files().at(path);
    require(bytes.size() >= 512 && retVal.decode(bytes.data()), "header did not decode");
    return retVal;
}

/* **
 * config - Default tiny two-file pool uses 256 payload bytes per file.
 */
static RecordingSessionConfig config()
{
    RecordingSessionConfig retVal;
    retVal._preparation = RecordingSessionConfig::Preparation::formatAndAllocate;
    retVal._fullPolicy = RecordingSessionConfig::FullPolicy::overwriteOldest;
    retVal._secondsPerFile = 1;
    retVal._fileCount = 2;
    retVal._sampleRate = 256;
    retVal._bytesPerSample = 1;
    retVal._sampleFormat = 2;
    return retVal;
}

/* **
 * headerChecks - Check exact prefix bytes, UTC, CRC, corruption rejection and
 * sample alignment.
 */
static void headerChecks()
{
    SessionFileHeader sessionFileHeader;
    sessionFileHeader._session = 1;
    sessionFileHeader._capacity = 300;
    sessionFileHeader._sampleRate = 100;
    sessionFileHeader._bytesPerSample = 3;
    sessionFileHeader._utcSeconds = utcSeconds();
    uint8_t data[512];
    require(sessionFileHeader.encode(data), "valid header rejected");
    require(data[0] == 1 && data[1] == 0 && data[2] == 2, "version/512-byte length changed");
    require(memcmp(data + 3, "2024-01-01T00:00:00Z", 20) == 0, "UTC encoding wrong");
    require(memcmp(data + 23, "Larry Lytle loves Lightning!", 28) == 0, "message wrong");
    require(SessionFileHeader::checksum(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xcbf43926,
        "CRC32 does not match known vector");
    SessionFileHeader decoded;
    require(decoded.decode(data), "header roundtrip failed");
    sessionFileHeader._lostBytes = 9;
    sessionFileHeader._lossFinal = true;
    require(sessionFileHeader.encode(data) && decoded.decode(data) && decoded._lostBytes == 9 && decoded._lossFinal,
        "loss metadata roundtrip");
    data[200] ^= 1;
    require(!decoded.decode(data), "header corruption not rejected");
    sessionFileHeader._capacity = 301;
    require(!sessionFileHeader.encode(data), "partial-sample capacity accepted");
}

/* **
 * sessionChecks - Fresh rotation, add preservation, reuse order and stop-full
 * acceptance boundary.
 */
static void sessionChecks()
{
    MemoryRecordingDisk memoryRecordingDisk;
    FileRecordingBackend fileRecordingBackend;
    require(fileRecordingBackend.initializeMemory(allocateRing, releaseRing, clockUs, utcSeconds), "memory init");
    auto recordingSessionConfig = config();
    require(fileRecordingBackend.prepForRecording(memoryRecordingDisk, recordingSessionConfig), "fresh prep");
    for (uint64_t offset = 0; offset < 1024; offset += 128)
    {
        require(offer(fileRecordingBackend, 128, offset) == 128, "fresh acceptance");
    }
    require(fileRecordingBackend.finishRecording() && fileRecordingBackend.verify(checkPayload), "fresh readback");
    const auto totals = fileRecordingBackend.getStatistics();
    require(totals._rollovers == 3 && totals._reuses == 2 && totals._written == 1024, "byte-based rotation counters");
    require(header("0:/RECORD/R0000000.BIN")._streamOffset == 512, "first reused file offset");
    MemoryRecordingDisk::files()["0:/NOTES.TXT"] = {1, 2, 3};
    const auto previous = MemoryRecordingDisk::files();

    recordingSessionConfig._preparation = RecordingSessionConfig::Preparation::addFiles;
    require(fileRecordingBackend.prepForRecording(memoryRecordingDisk, recordingSessionConfig), "add prep");
    require(offer(fileRecordingBackend, 128, 0) == 128, "add acceptance");
    require(fileRecordingBackend.finishRecording() && fileRecordingBackend.verify(checkPayload), "add readback");
    require(MemoryRecordingDisk::files().size() == previous.size() + 2, "add did not create exactly two");
    for (const auto& old : previous)
    {
        require(MemoryRecordingDisk::files().at(old.first) == old.second, "old file changed");
    }
    require(header("0:/RECORD/R0000002.BIN")._session == 2, "new session ID");
    require(header("0:/RECORD/R0000003.BIN")._state == 0, "unused slot not retained");

    // Duration/count are allocation parameters, ignored when reusing existing slots.
    recordingSessionConfig._preparation = RecordingSessionConfig::Preparation::useExisting;
    recordingSessionConfig._secondsPerFile = 0;
    recordingSessionConfig._fileCount = 0;
    recordingSessionConfig._fullPolicy = RecordingSessionConfig::FullPolicy::stopWhenFull;
    const auto beforeReuse = MemoryRecordingDisk::files();
    require(fileRecordingBackend.prepForRecording(memoryRecordingDisk, recordingSessionConfig), "useExisting prep");
    require(offer(fileRecordingBackend, 400, 0) == 256 && fileRecordingBackend.getFull(), "stop-full final prefix");
    require(fileRecordingBackend.finishRecording() && fileRecordingBackend.verify(checkPayload), "stop-full drain");
    require(header("0:/RECORD/R0000003.BIN")._session == 3, "unused slot was not selected first");
    for (const auto& old : beforeReuse)
    {
        if (old.first != "0:/RECORD/R0000003.BIN")
        {
            require(MemoryRecordingDisk::files().at(old.first) == old.second, "stop-full overwrote old file");
        }
    }
    require(!fileRecordingBackend.prepForRecording(memoryRecordingDisk, recordingSessionConfig), "full stop-only pool accepted");
    recordingSessionConfig._fullPolicy = RecordingSessionConfig::FullPolicy::overwriteOldest;
    require(fileRecordingBackend.prepForRecording(memoryRecordingDisk, recordingSessionConfig), "oldest prep");
    require(offer(fileRecordingBackend, 16, 0) == 16 && fileRecordingBackend.finishRecording(), "oldest write");
    require(header("0:/RECORD/R0000000.BIN")._session == 4, "oldest session/sequence not selected");
}

/* **
 * existingPoolChecks - Preallocate unused slots, then fill them through
 * useExisting without new files.
 */
static void existingPoolChecks()
{
    MemoryRecordingDisk memoryRecordingDisk;
    FileRecordingBackend fileRecordingBackend;
    require(fileRecordingBackend.initializeMemory(allocateRing, releaseRing, clockUs, utcSeconds), "pool memory");
    auto recordingSessionConfig = config();
    recordingSessionConfig._fullPolicy = RecordingSessionConfig::FullPolicy::stopWhenFull;
    require(fileRecordingBackend.prepForRecording(memoryRecordingDisk, recordingSessionConfig), "pool baseline prep");
    require(offer(fileRecordingBackend, 512, 0) == 512 && fileRecordingBackend.finishRecording(), "pool baseline write");
    const auto oldFiles = MemoryRecordingDisk::files();
    recordingSessionConfig._preparation = RecordingSessionConfig::Preparation::useExisting;
    require(!fileRecordingBackend.prepForRecording(memoryRecordingDisk, recordingSessionConfig), "used pool not refused");
    require(MemoryRecordingDisk::files() == oldFiles, "refused start changed old files");
    recordingSessionConfig._preparation = RecordingSessionConfig::Preparation::addFiles;
    recordingSessionConfig._fileCount = 5;
    require(fileRecordingBackend.preallocate(memoryRecordingDisk, recordingSessionConfig), "unused allocation");
    const auto allocated = MemoryRecordingDisk::files();
    require(allocated.size() == oldFiles.size() + 5, "wrong allocation count");
    for (const auto& entry : allocated)
    {
        if (!oldFiles.count(entry.first))
        {
            require(header(entry.first.c_str())._state == 0, "allocation started a recording");
        }
    }
    recordingSessionConfig._preparation = RecordingSessionConfig::Preparation::useExisting;
    recordingSessionConfig._fileCount = 0;
    recordingSessionConfig._secondsPerFile = 0;
    require(fileRecordingBackend.prepForRecording(memoryRecordingDisk, recordingSessionConfig), "unused pool selection");
    uint64_t accepted = 0;
    while (!fileRecordingBackend.getFull() && accepted < 1500)
    {
        accepted += offer(fileRecordingBackend, 100, accepted);
    }
    require(fileRecordingBackend.getFull() && fileRecordingBackend.finishRecording(), "pool full stop");
    const auto statistics = fileRecordingBackend.getStatistics();
    require(statistics._written == 1280 && statistics._rollovers == 4 && statistics._reuses == 0, "pool capacity/changes");
    require(fileRecordingBackend.verify(checkPayload), "pool payload verification");
    require(MemoryRecordingDisk::files().size() == allocated.size(), "useExisting added files");
    for (const auto& entry : allocated)
    {
        require(MemoryRecordingDisk::files().at(entry.first).size() == entry.second.size(), "useExisting resized a file");
    }
    for (const auto& entry : oldFiles)
    {
        require(MemoryRecordingDisk::files().at(entry.first) == entry.second, "outside pool changed");
    }
    const auto fullFiles = MemoryRecordingDisk::files();
    require(!fileRecordingBackend.prepForRecording(memoryRecordingDisk, recordingSessionConfig), "full pool restarted");
    require(MemoryRecordingDisk::files() == fullFiles, "full refusal changed files");
}

/* **
 * overflowChecks - Overflow recovery must not rotate files; final headers report
 * session loss.
 */
static void overflowChecks()
{
    MemoryRecordingDisk memoryRecordingDisk;
    FileRecordingBackend fileRecordingBackend;
    require(fileRecordingBackend.initializeMemory(allocateRing, releaseRing, clockUs, utcSeconds), "overflow memory");
    auto recordingSessionConfig = config();
    recordingSessionConfig._sampleRate = 4096;
    require(fileRecordingBackend.prepForRecording(memoryRecordingDisk, recordingSessionConfig), "overflow prep");
    require(offer(fileRecordingBackend, 600, 0, false) == 600, "first batch");
    require(offer(fileRecordingBackend, 600, 600, false) == 0 && !fileRecordingBackend.getFailed(), "overflow must recover");
    while (fileRecordingBackend.getQueuedBytes() >= BUFFERED_WRITER_BLOCK_BYTES)
    {
        require(fileRecordingBackend.pump(), "overflow drain");
    }
    require(offer(fileRecordingBackend, 600, 600) == 600, "recovery batch");
    require(fileRecordingBackend.finishRecording() && fileRecordingBackend.verify(checkPayload), "recovered readback");
    const auto recovered = header("0:/RECORD/R0000000.BIN");
    require(recovered._validBytes == 1200 && recovered._lostBytes == 600 && recovered._lossFinal,
        "recovered payload or session loss total");
    require(fileRecordingBackend.getStatistics()._rollovers == 0, "overflow caused an unnecessary file change");
    require(header("0:/RECORD/R0000001.BIN")._state == 0, "overflow used an unneeded file");

    // Repeated missed input must not exhaust partly filled slots or discard
    // accepted bytes under stopWhenFull. Only saved payload consumes capacity.
    recordingSessionConfig._fullPolicy = RecordingSessionConfig::FullPolicy::stopWhenFull;
    require(fileRecordingBackend.prepForRecording(memoryRecordingDisk, recordingSessionConfig), "gap/full prep");
    uint64_t accepted = 0;
    for (unsigned batch = 0; batch < 100; ++batch)
    {
        fileRecordingBackend.skipInput(1);
        const uint32_t saved = offer(fileRecordingBackend, 100, accepted);
        accepted += saved;
        if (fileRecordingBackend.getFull())
        {
            break;
        }
    }
    require(fileRecordingBackend.finishRecording() && fileRecordingBackend.getFull(), "gap/full completion");
    const auto fullTotals = fileRecordingBackend.getStatistics();
    require(accepted == 8192 && fullTotals._written == 8192 && fullTotals._rollovers == 1,
        "gaps consumed file capacity or lost accepted data");
    require(fullTotals._lostBytes == 82, "missed input loss accounting");
    require(fileRecordingBackend.verify(checkPayload), "gap/full retained bytes");
    for (const char* path : {"0:/RECORD/R0000000.BIN", "0:/RECORD/R0000001.BIN"})
    {
        const auto finalHeader = header(path);
        require(finalHeader._lostBytes == 82 && finalHeader._lossFinal, "retained session loss total not finalized");
    }
}

/* **
 * failureChecks - Short writes and sync errors remain failures; a partial file
 * is not marked complete.
 */
static void failureChecks()
{
    for (int fault = 0; fault < 3; ++fault)
    {
        MemoryRecordingDisk memoryRecordingDisk;
        FileRecordingBackend fileRecordingBackend;
        require(fileRecordingBackend.initializeMemory(allocateRing, releaseRing, clockUs, utcSeconds), "fault memory");
        auto recordingSessionConfig = config();
        recordingSessionConfig._sampleRate = 4096;
        require(fileRecordingBackend.prepForRecording(memoryRecordingDisk, recordingSessionConfig), "fault prep");
        require(offer(fileRecordingBackend, 300, 0, false) == 300, "fault acceptance");
        if (fault == 2)
        {
            MemoryRecordingDisk::failSync();
        }
        else
        {
            MemoryRecordingDisk::failWrite(fault == 1);
        }
        require(!fileRecordingBackend.finishRecording() && fileRecordingBackend.getFailed(), "I/O failure hidden");
        require(header("0:/RECORD/R0000000.BIN")._state == 1, "failed payload marked complete");
    }

    // An allocation/header failure during addFiles cannot erase existing files.
    MemoryRecordingDisk memoryRecordingDisk;
    FileRecordingBackend fileRecordingBackend;
    require(fileRecordingBackend.initializeMemory(allocateRing, releaseRing, clockUs, utcSeconds), "add fault memory");
    auto recordingSessionConfig = config();
    require(fileRecordingBackend.prepForRecording(memoryRecordingDisk, recordingSessionConfig) && fileRecordingBackend.finishRecording(), "add fault baseline");
    const auto originals = MemoryRecordingDisk::files();
    recordingSessionConfig._preparation = RecordingSessionConfig::Preparation::addFiles;
    MemoryRecordingDisk::failWrite(true);
    require(!fileRecordingBackend.prepForRecording(memoryRecordingDisk, recordingSessionConfig), "short allocated header accepted");
    for (const auto& old : originals)
    {
        require(MemoryRecordingDisk::files().at(old.first) == old.second, "failed add changed old files");
    }
}

/* **
 * collectReadback - Model an ADC observer: require continuous offsets, copy only
 * card-read payload.
 */
static bool collectReadback(void* context, const uint8_t* data, uint32_t bytes, uint64_t offset)
{
    auto& collected = *static_cast<std::vector<uint8_t>*>(context);
    const bool retVal = offset == collected.size();
    if (retVal)
    {
        collected.insert(collected.end(), data, data + bytes);
    }
    return retVal;
}

/* **
 * adcReadbackChecks - Exercise three-byte samples, partial final files, ordered
 * callbacks and corruption.
 */
static void adcReadbackChecks()
{
    MemoryRecordingDisk memoryRecordingDisk;
    FileRecordingBackend fileRecordingBackend;
    require(fileRecordingBackend.initializeMemory(allocateRing, releaseRing, clockUs, utcSeconds), "ADC memory");
    auto recordingSessionConfig = config();
    recordingSessionConfig._sampleRate = 100;
    recordingSessionConfig._bytesPerSample = 3;
    recordingSessionConfig._sampleFormat = 1;
    recordingSessionConfig._fullPolicy = RecordingSessionConfig::FullPolicy::stopWhenFull;
    require(fileRecordingBackend.prepForRecording(memoryRecordingDisk, recordingSessionConfig), "ADC prepare");
    require(offer(fileRecordingBackend, 450, 0) == 450 && offer(fileRecordingBackend, 123, 450) == 123, "ADC submit");
    while (fileRecordingBackend.getQueuedBytes())
    {
        require(fileRecordingBackend.pump(true), "ADC short drain");
    }
    require(fileRecordingBackend.finishRecording(), "ADC finish");
    std::vector<uint8_t> collected;
    require(fileRecordingBackend.verify(collectReadback, &collected) && collected.size() == 573, "ADC joined readback");
    require(checkPayload(collected.data(), static_cast<uint32_t>(collected.size()), 0), "ADC data changed at boundary");
    require(header("0:/RECORD/R0000000.BIN")._validBytes == 300 &&
                header("0:/RECORD/R0000001.BIN")._validBytes == 273,
        "ADC final length");
    require(fileRecordingBackend.getStatistics()._rollovers == 1, "ADC boundary not exercised");
    const uint32_t submittedCrc = SessionFileHeader::checksum(collected.data(), collected.size());

    // An ADC has no known pattern. Its caller must compare the accumulated
    // checksum; backend header verification alone cannot certify analog bytes.
    MemoryRecordingDisk::files().at("0:/RECORD/R0000001.BIN")[512] ^= 1;
    collected.clear();
    require(fileRecordingBackend.verify(collectReadback, &collected), "ADC reread");
    require(SessionFileHeader::checksum(collected.data(), collected.size()) != submittedCrc, "ADC corruption hidden");

    // Physical slot order differs after reuse. Saved-stream order still wins;
    // a whole-session observer must reject an overwritten beginning.
    recordingSessionConfig._fullPolicy = RecordingSessionConfig::FullPolicy::overwriteOldest;
    require(fileRecordingBackend.prepForRecording(memoryRecordingDisk, recordingSessionConfig), "ordered prepare");
    for (uint32_t offset = 0; offset < 900; offset += 300)
    {
        require(offer(fileRecordingBackend, 300, offset) == 300, "ordered offer");
    }
    require(fileRecordingBackend.finishRecording(), "ordered finish");
    collected.clear();
    require(!fileRecordingBackend.verify(collectReadback, &collected), "overwritten beginning accepted");
    collected.resize(300);
    require(fileRecordingBackend.verify(collectReadback, &collected) && collected.size() == 900, "retained stream order");
    require(checkPayload(collected.data() + 300, 600, 300), "retained stream data");
}

/* **
 * testChecksum - Incremental test checksum; chunk boundaries must not change its
 * value.
 */
static uint32_t testChecksum(uint32_t crc, const uint8_t* data, uint32_t bytes)
{
    for (uint32_t iByte = 0; iByte < bytes; ++iByte)
    {
        crc = crc * 33 + data[iByte];
    }
    return crc;
}

/* **
 * retainedChecksumChecks - Check surviving generations independently of disk
 * writes. Unequal capacities and split submissions expose rollover errors.
 * Readback must reject changed bytes and wrong offsets, even after several
 * reuses.
 */
static void retainedChecksumChecks()
{
    const uint8_t data[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18};
    RetainedPayloadChecksums retainedPayloadChecksums;
    require(!retainedPayloadChecksums.prepare({}, testChecksum), "empty CRC pool accepted");
    require(!retainedPayloadChecksums.prepare({0}, testChecksum), "zero capacity accepted");
    for (unsigned length : {3U, 8U, 19U})
    {
        require(retainedPayloadChecksums.prepare({3, 5}, testChecksum), "CRC prepare");
        retainedPayloadChecksums.accept(data, 2);
        retainedPayloadChecksums.accept(data + 2, length - 2);
        const uint64_t first = retainedPayloadChecksums.beginReadback();
        require(first == (length == 19 ? 11U : 0U), "wrong surviving offset");
        require(!retainedPayloadChecksums.getVerified(), "premature CRC success");
        for (uint64_t offset = first; offset < length; ++offset)
        {
            require(retainedPayloadChecksums.check(data + offset, 1, offset), "split CRC readback");
        }
        require(retainedPayloadChecksums.getVerified(), "CRC completion missing");
    }

    // A partial final generation still needs its checksum; corrupt its final byte.
    require(retainedPayloadChecksums.prepare({3, 5}, testChecksum), "CRC reprepare");
    retainedPayloadChecksums.accept(data, 18);
    require(retainedPayloadChecksums.beginReadback() == 11, "partial generation offset");
    require(retainedPayloadChecksums.check(data + 11, 6, 11), "partial read");
    const uint8_t corrupt = 99;
    require(!retainedPayloadChecksums.check(&corrupt, 1, 17), "corruption accepted");
    require(!retainedPayloadChecksums.getVerified(), "corrupt completion");
    require(retainedPayloadChecksums.prepare({3, 5}, testChecksum), "offset prepare");
    retainedPayloadChecksums.accept(data, 8);
    retainedPayloadChecksums.beginReadback();
    require(!retainedPayloadChecksums.check(data + 1, 1, 1), "missing byte accepted");
}

/* **
 * main - PC entry: exception text identifies the failed invariant; zero means
 * all passed.
 */
int main()
{
    int retVal = 0;
    try
    {
        headerChecks();
        sessionChecks();
        existingPoolChecks();
        overflowChecks();
        failureChecks();
        adcReadbackChecks();
        retainedChecksumChecks();
        puts("Session backend checks passed");
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "Session backend FAIL: %s\n", error.what());
        retVal = 1;
    }
    return retVal;
}
