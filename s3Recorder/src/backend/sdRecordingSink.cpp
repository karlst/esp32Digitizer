/**
 * @file sdRecordingSink.cpp
 * @brief Keep card-specific details out of the reusable RAM buffer/writer.
 * Reading path: mount(), open(), write(), close(). Each part begins with a
 * 512-byte header; data after it is described by that header, not this filesystem.
 * An open/incomplete header can identify unfinished work after a fault; power
 * loss can also damage filesystem metadata, so no marker guarantees recovery.
 *
 * This is the card-specific half of recording. bufferedWriter owns queued RAM
 * bytes; this object owns the actual file and filesystem. recordingService is
 * the only task allowed to call us. None of these functions runs inside the
 * digitizer interrupt. To trace a sample to disk, follow write(); to understand
 * what Start/Stop do to files, follow open()/openPart() and close()/closePart().
 */
#include "sdRecordingSink.h"
#include "recordingSpace.h"
#include <cstring>
#include <cstdio>
#if S3_PREALLOCATED_PAYLOAD_BYTES
#include <esp_timer.h>
#endif
#if S3_CHOKE_TEST
#include "chokeTest.h"

// Different magic prevents synthetic samples being mistaken for measured data.
static constexpr const char* recordingMagic = "S3CHK001";
#else
static constexpr const char* recordingMagic = "S3REC001";
#endif

/**
 * @brief Put an unsigned integer in the header, least significant byte first.
 * Used only when building file headers, never in the ADC interrupt.
 * destination points at the field being filled; bytes is its width (4 or 8).
 * For each successive byte, shift away another eight bits and keep the lowest
 * remaining eight. This defines a portable file format independent of CPU order.
 */
static void putLittle(uint8_t* destination, uint64_t value, size_t bytes)
{
    for (size_t index = 0; index < bytes; ++index)
    {
        destination[index] = static_cast<uint8_t>(value >> (8 * index));
    }
}

/**
 * @brief Initialize the dedicated SD bus and mount existing formatting only.
 * A failed mount is reported as I/O failure: it can mean missing card, wiring,
 * power, or unsupported formatting; software cannot reliably distinguish them.
 * Repeated calls reuse a successful mount. Card removal during use is an error.
 *
 * Called by the storage thread at startup and before opening/deleting files.
 * Mount means recognize the EXISTING filesystem and make its directories usable;
 * it does not format the card. FsVolume handles FAT and exFAT. The native
 * SD controller moves four data bits per clock, independently of the ADC SPI
 * controller. See sdmmcBlockDevice::begin() for the six signal assignments.
 * @return True means this object has mounted successfully. It is not a fresh card
 * presence test on every call; removing an already-mounted card causes later I/O
 * failures. Recovery from removal is not implemented as automatic hot swapping.
 */
bool sdRecordingSink::mount()
{
    if (!mounted)
    {
        // Initialize the physical link, then interpret partition 1. Some cards
        // store a filesystem directly at sector zero instead of in a partition;
        // try that existing layout too. Neither attempt formats or erases data.
        if (device.begin())
        {
            mounted = filesystem.begin(&device);
            if (!mounted)
            {
                mounted = filesystem.begin(&device, true, 0);
            }
            if (!mounted)
            {
                device.end();
            }
        }
        if (mounted)
        {
            Serial.printf("SD mounted: filesystem=%s capacity=%llu bytes\n",
                filesystem.fatType() == 64 ? "exFAT" : "FAT",
                static_cast<unsigned long long>(filesystem.clusterCount()) * filesystem.bytesPerCluster());
        }
    }
    cardState = mounted ? recordingStatus::ready : recordingStatus::ioFailure;
    const bool retVal = mounted;
    return retVal;
}

/**
 * @brief Recognize only files created by this recorder, including large IDs.
 * Full consumption plus the canonical spelling prevents deleting similarly
 * named unrelated files. No directory traversal or arbitrary path is accepted.
 *
 * Used while choosing the next session number and while filtering deletion.
 * name is a directory entry's filename, not a complete path. Session and part
 * outputs are meaningful only if true is returned. Both IDs must be nonzero.
 * For example recording_000002_001.bin matches; recording_2_1.bin and a name with
 * extra characters do not. The six/three digits are minimum padding, not limits.
 * This recognizes a filename convention; it does not inspect the file's contents.
 */
bool sdRecordingSink::recordingName(const char* name, uint32_t& foundSession, uint32_t& foundPart)
{
    // Parse both numbers and record where scanning stopped (%n). Checking that
    // position against the string terminator rejects a valid prefix plus junk.
    unsigned long sessionValue = 0, partValue = 0;
    int end = 0;
    bool retVal = std::sscanf(name, "recording_%lu_%lu.bin%n", &sessionValue, &partValue, &end) == 2 &&
        name[end] == '\0' && sessionValue && partValue;
    if (retVal)
    {
        // Rebuild the exact spelling our recorder generates, including zero padding.
        // Only the exact filename convention is eligible for automatic deletion.
        char canonical[64];
        std::snprintf(canonical, sizeof(canonical), "recording_%06lu_%03lu.bin", sessionValue, partValue);
        retVal = std::strcmp(name, canonical) == 0;
        foundSession = static_cast<uint32_t>(sessionValue);
        foundPart = static_cast<uint32_t>(partValue);
    }
    return retVal;
}

/**
 * @brief Create a new session without replacing previous recordings.
 * Scan existing names to choose the next number, then use O_EXCL so even an
 * unexpected collision fails safely. No samples may be submitted before success.
 *
 * recordingService calls this before allowing ADC samples into the ring.
 * A session is one Start-to-Stop recording; a part is one file within that session.
 * @param sampleRate Requested ADC measurements per second, saved in each header.
 * In the temporary Choke build this is zero: the rate varies according to the
 * ramp parameters stored in its distinct S3CHK001 header.
 * @return True only after the first part and its initial header are ready. False
 * leaves acquisition stopped; it does not fall back to recording nowhere.
 * Existing names determine the next number after reboot. If all recordings were
 * deleted, numbering may restart. This is a file naming scheme, not a permanent ID.
 */
bool sdRecordingSink::open(uint32_t sampleRate)
{
    bool retVal = !file.isOpen() && mount();
    if (retVal && !filesystem.exists("/recordings"))
    {
        retVal = filesystem.mkdir("/recordings");
    }

    // Inspect filenames only; do not read or alter old sample data. An existing
    // directory with a recorder-shaped name also reserves that number safely.
    uint32_t highest = 0;
    FsFile directory;
    retVal = retVal && directory.open(&filesystem, "/recordings", O_RDONLY) && directory.isDir();
    if (retVal)
    {
        FsFile entry;
        while (entry.openNext(&directory, O_RDONLY))
        {
            char name[256];
            uint32_t foundSession = 0, foundPart = 0;
            if (entry.getName(name, sizeof(name)) && recordingName(name, foundSession, foundPart) && foundSession > highest)
            {
                highest = foundSession;
            }
            retVal = entry.close() && retVal;
        }

        // End-of-directory is normal; a read error must not masquerade as an
        // empty directory. O_EXCL below additionally protects existing names.
        retVal = directory.getError() == 0 && retVal;
    }
    if (directory.isOpen())
    {
        retVal = directory.close() && retVal;
    }

    // Avoid wrapping the last possible 32-bit session number back to zero.
    // Reset byte accounting only when beginning a new session, not on part rollover.
    if (retVal && highest != UINT32_MAX)
    {
        session = highest + 1; part = 1; rate = sampleRate;
        payloadBytes = 0; headerBytes = 0; partStart = 0; allocatedBytes = 0;
        retVal = openPart();
    }
    else
    {
        retVal = false;
    }
    return retVal;
}

/**
 * @brief Write/rewrite the fixed header and restore the append position.
 * Layout (little endian): magic[8], header size u32, header version u32,
 * format ID u32 (1=signed int32), rate u32, word bytes u32, disposition u32
 * (0=open, 1=closed normally, 2=incomplete), part payload bytes u64,
 * first sample index u64, session u32, part u32; remaining bytes reserved zero.
 * Choke builds instead use S3CHK001 magic, rate zero, and three additional fields
 * at 56/60/64: initial stored bits/s, increment bits/s, and stage milliseconds.
 * Completed writes count toward bytesWritten even when rewriting this header.
 *
 * A header describes the bytes that follow it so a later reader knows how to
 * interpret the file. Payload means only the sample bytes, excluding this header.
 * Called when a part opens and again when it closes with the final payload count.
 * disposition 0 means opened but not finalized, 1 normal completion, 2 incomplete.
 * The fields occupy fixed byte offsets; shared/recordingFileFormat.md is the table.
 * @return True if the full header was accepted and the append position restored.
 * This method does not flush by itself; openPart()/closePart() decide when to sync.
 * Writing the final header is counted as another write even though it replaces
 * existing bytes. Therefore UI bytesWritten can exceed final file size.
 */
bool sdRecordingSink::header(uint32_t disposition)
{
    // Start with all reserved bytes zero. The 8-byte magic identifies this file
    // as our recording format; subsequent fields describe its version and data.
    uint8_t bytes[512] = {};
    std::memcpy(bytes, recordingMagic, 8);
    putLittle(bytes + 8, 512, 4); putLittle(bytes + 12, 1, 4);
    putLittle(bytes + 16, 1, 4); putLittle(bytes + 20, rate, 4);
    putLittle(bytes + 24, 4, 4); putLittle(bytes + 28, disposition, 4);
    putLittle(bytes + 32, partBytes, 8); putLittle(bytes + 40, partStart / 4, 8);
    putLittle(bytes + 48, session, 4); putLittle(bytes + 52, part, 4);
#if S3_CHOKE_TEST

    // These are requested stored-bit rates, not an assertion of achieved timing.
    // The synthetic counter payload remains the ordinary four-byte signed format.
    putLittle(bytes + 56, chokeTest::initialBps, 4);
    putLittle(bytes + 60, chokeTest::fixedRate ? 0 : chokeTest::incrementBps, 4);
    putLittle(bytes + 64, chokeTest::stageUs / 1000, 4);
#endif

    // Move to the first byte so finalization updates the header instead of
    // appending a second header among the samples.
    bool retVal = file.seekSet(0);
    if (retVal)
    {
        const size_t written = file.write(bytes, sizeof(bytes));
        if (written > 0)
        {
            headerBytes += written;
            accountGrowth();
        }
        retVal = written == sizeof(bytes);
    }
    if (retVal)
    {
        // Restore the end-of-payload position. Any subsequent write must append
        // samples after the header and all earlier data, not overwrite the header.
        retVal = file.seekSet(512 + partBytes);
    }
    return retVal;
}

/**
 * @brief Exclusively create one part and synchronize its initial open header.
 * Called for the first file and whenever write() rolls over to another part.
 * O_CREAT creates a missing file, O_EXCL refuses to replace an existing one, and
 * O_RDWR permits both reading and writing. These flags are combined with bitwise OR.
 * The 512-byte initial header is synchronized before returning success. If that
 * fails, close any opened handle; a partial file may remain for later inspection.
 * No caller may submit new recording data until the initial open has succeeded.
 * The preallocated diagnostic reserves one contiguous exFAT file BEFORE writing
 * its header (SdFat requires an empty file). Reservation changes allocation
 * metadata; it does not fill gigabytes with junk. Failure rejects Start rather
 * than falling back to allocation during capture. Its time belongs to openUs.
 */
bool sdRecordingSink::openPart()
{
    char path[96];
    std::snprintf(path, sizeof(path), "/recordings/recording_%06lu_%03lu.bin",
        static_cast<unsigned long>(session), static_cast<unsigned long>(part));
    const bool opened = file.open(&filesystem, path, O_CREAT | O_EXCL | O_RDWR);
    partBytes = 0;
    partAllocated = 0;
    bool retVal = opened;
#if S3_PREALLOCATED_PAYLOAD_BYTES
    const uint64_t reserveBytes = recordingConfig::reservePayloadBytes + 512ULL;
    const int64_t began = esp_timer_get_time();
    Serial.printf("SD reserve-start: bytes=%llu\n", static_cast<unsigned long long>(reserveBytes));
    retVal = retVal && filesystem.fatType() == 64 && file.preAllocate(reserveBytes) &&
        file.isContiguous() && file.fileSize() == reserveBytes;
    Serial.printf("SD reserve-end: success=%u bytes=%llu durationUs=%llu\n", retVal,
        static_cast<unsigned long long>(reserveBytes),
        static_cast<unsigned long long>(esp_timer_get_time() - began));
#endif
    retVal = retVal && header(0) && file.sync();
    if (!retVal && file.isOpen())
    {
        // This handle belongs only to the new, exclusively created file. Release
        // any reservation after failed startup; never truncate an older recording.
#if S3_PREALLOCATED_PAYLOAD_BYTES
        file.truncate(0);
#endif
        file.close();
    }
    return retVal;
}

/**
 * @brief Append bytes, opening a new part before the agreed size limit.
 * Returns only payload bytes actually accepted. Rollover finishes the previous
 * part first; if that fails, stop instead of hiding a possibly incomplete file.
 *
 * This is the byteSink implementation reached by bufferedWriter::pump(). It runs
 * in the storage task and may wait on the card. data points directly into the ring;
 * length is the number of contiguous bytes pump selected. Do not retain that
 * pointer after returning: a successful return lets the ring reuse those bytes.
 * The return value is the library's accepted byte count; less than length makes
 * the reusable writer stop normal writing and report failure. It is not a claim
 * that the card's own cache would survive a sudden loss of power.
 * While this call closes/opens a part, the producer can continue filling RAM.
 */
size_t sdRecordingSink::write(const uint8_t* data, size_t length)
{
    size_t retVal = 0;
    bool writable = file.isOpen();
#if S3_PREALLOCATED_PAYLOAD_BYTES

    // A single preallocated exFAT file covers the complete timed test. Refuse a
    // write beyond its reservation instead of silently allocating more clusters.
    // Ordinary builds below retain their existing approximately 1-GiB parts.
    writable = writable && partBytes <= recordingConfig::reservePayloadBytes &&
        length <= recordingConfig::reservePayloadBytes - partBytes;
#else

    // Check the NEXT write before crossing the size limit, including the header.
    // A split must occur between complete submissions, never in the middle of one.
    if (writable && 512 + partBytes + length >= partLimit)
    {
        writable = closePart(true) && part != UINT32_MAX;
        if (writable)
        {
            // Keep the same session, increment the part, and remember how much payload
            // preceded it. Its header first-sample index is partStart / four bytes.
            ++part; partStart = payloadBytes;
            writable = openPart();
        }
    }
#endif
    if (writable)
    {
        const size_t written = file.write(data, length);
        if (written > 0)
        {
            retVal = static_cast<size_t>(written);
            partBytes += retVal; payloadBytes += retVal;
            accountGrowth();

            // Remember the last four accepted bytes for the close-time readback.
            // Only normally completed parts use this as a complete-sample comparison.
            if (written >= sizeof(lastWord))
            {
                std::memcpy(lastWord, data + written - sizeof(lastWord), sizeof(lastWord));
            }
        }
    }
    return retVal;
}

/**
 * @brief Ask the filesystem to synchronize pending data; failure is observable.
 * bufferedWriter::finish() calls this after emptying the ring. closePart() also
 * uses it before and after updating the header. sync asks SdFat to write cached
 * file data and filesystem bookkeeping. A successful write alone is weaker than
 * a successful write followed by sync; neither can guarantee sudden-power-loss
 * survival inside the card. False also covers calling this without an open file.
 */
bool sdRecordingSink::flush()
{
    const bool retVal = file.isOpen() && file.sync();
    return retVal;
}

/**
 * @brief Finalize a part; do not mark complete unless its earlier data synchronized.
 * A sudden power loss can still defeat the card's internal cache. These markers
 * describe successful software calls, not a promise against unplugging power.
 *
 * Called after the ring drains, or by write() before opening another part.
 * Order matters: sync payload, write final header, sync that header, close handle,
 * then read back a small amount for verification. Even on failure, attempt the
 * later synchronization/close so a file handle is not deliberately left open.
 * complete is the caller's assessment of acquisition/recording success; we also
 * require the payload sync to succeed before writing a normal-completion marker.
 * A failure while updating/syncing the marker can itself leave an uncertain header;
 * status remains failed even if a reader later happens to see disposition 1.
 */
bool sdRecordingSink::closePart(bool complete)
{
    bool retVal = file.isOpen();
    if (file.isOpen())
    {
        // Synchronize earlier samples before claiming normal completion. A failed
        // sync forces an incomplete marker even if the caller requested complete=true.
        bool dataSaved = flush();
#if S3_PREALLOCATED_PAYLOAD_BYTES

        // Producer has stopped and queued data has drained. Drop only the unused
        // reserved tail, so readers see header plus actual samples after an early
        // Stop/failure. Failure here prevents claiming normal completion.
        const bool trimmed = file.truncate(512 + partBytes);
        dataSaved = trimmed && dataSaved;
#endif
        retVal = header(complete && dataSaved ? 1 : 2) && dataSaved;

        // Attempt both sync and close regardless of the earlier result; only report
        // success if every required stage succeeded.
        const bool headerSaved = flush();
        const bool closed = file.close();
        retVal = retVal && headerSaved && closed;
        if (retVal)
        {
            retVal = verifyPart(complete ? 1 : 2);
        }
    }
    return retVal;
}

/**
 * @brief Close the final part, preserving incomplete status after an ADC/storage fault.
 * The storage service calls this for final Stop/fault cleanup. Already closed
 * returns true, making repeated cleanup harmless. False means saving/finalization
 * or its readback failed. complete=false still saves earlier good bytes, but tells
 * the header that this recording did not finish normally.
 */
bool sdRecordingSink::close(bool complete)
{
    const bool retVal = !file.isOpen() || closePart(complete);
    return retVal;
}

/**
 * @brief Delete only our canonical recording files, and only while no file is open.
 * Called solely for an explicit delete command after acquisition has stopped.
 * Unrelated files and subdirectories are preserved; there is no format operation.
 *
 * The browser confirmation and acquisition-stopped check happen before this call.
 * This method also requires no open recording file. deleted is reset on entry and
 * counts successful deletions even if a later deletion fails. A missing recordings
 * directory is treated as nothing to delete, not as a request to create it.
 * Only regular files whose names match recordingName() are removed. No recursion,
 * formatting, or deletion of arbitrary card contents occurs. Name matching alone
 * cannot distinguish our files from someone else's identically named files.
 */
bool sdRecordingSink::deleteRecordings(uint64_t& deleted)
{
    deleted = 0;
    bool retVal = !file.isOpen() && mount();
    FsFile directory;
    if (retVal && filesystem.exists("/recordings"))
    {
        retVal = directory.open(&filesystem, "/recordings", O_RDONLY) && directory.isDir();
        FsFile entry;
        while (retVal && entry.openNext(&directory, O_RDONLY))
        {
            char name[256];
            uint32_t foundSession = 0, foundPart = 0;
            if (entry.isFile() && entry.getName(name, sizeof(name)) &&
                recordingName(name, foundSession, foundPart))
            {
                // Enumerate read-only so unrelated subdirectories can be skipped.
                // Close our regular file before removing its validated relative name.
                retVal = entry.close() && directory.remove(name);
                if (retVal)
                {
                    ++deleted;
                }
            }
            if (entry.isOpen())
            {
                retVal = entry.close() && retVal;
            }
        }
        retVal = directory.getError() == 0 && retVal;
    }
    if (directory.isOpen())
    {
        retVal = directory.close() && retVal;
    }
    return retVal;
}

/**
 * @brief Reopen a closed part and check its header, length and final sample word.
 * This bounded readback catches layout/seek/truncation mistakes without rereading
 * gigabytes on every Stop. It is not a full-payload checksum or power-loss test.
 * Close-time latency includes this small verification and remains visible in P3.
 *
 * Called only after closePart() has successfully closed the writer's handle.
 * Build the expected header from the recording metadata, open a separate read-only
 * handle, and require the stored length to match header plus payload. For a normally
 * closed nonempty part, compare the final four bytes to the last word we submitted.
 * Partial/failed payloads are not expected to have a complete final word.
 * A true result verifies these limited checks, NOT every byte in the middle of the
 * file. The printed result goes to USB diagnostics, not the Feather UART protocol.
 */
bool sdRecordingSink::verifyPart(uint32_t disposition)
{
    char path[96];
    std::snprintf(path, sizeof(path), "/recordings/recording_%06lu_%03lu.bin",
        static_cast<unsigned long>(session), static_cast<unsigned long>(part));
    FsFile check;

    // Only the first 56 header bytes currently contain defined fields. Reserved
    // bytes are not compared here, nor is the entire payload reread.
    uint8_t actual[56] = {}, expected[56] = {};
    std::memcpy(expected, recordingMagic, 8);
    putLittle(expected + 8, 512, 4); putLittle(expected + 12, 1, 4);
    putLittle(expected + 16, 1, 4); putLittle(expected + 20, rate, 4);
    putLittle(expected + 24, 4, 4); putLittle(expected + 28, disposition, 4);
    putLittle(expected + 32, partBytes, 8); putLittle(expected + 40, partStart / 4, 8);
    putLittle(expected + 48, session, 4); putLittle(expected + 52, part, 4);

    // Opening read-only cannot modify the saved samples. Short reads, unexpected
    // file length, or mismatched header bytes all make verification fail.
    bool retVal = check.open(&filesystem, path, O_RDONLY) && check.fileSize() == 512 + partBytes &&
        check.read(actual, sizeof(actual)) == sizeof(actual) &&
        std::memcmp(actual, expected, sizeof(actual)) == 0;
    if (retVal && disposition == 1 && partBytes >= 4)
    {
        // Seek directly to the final sample. This check takes bounded work even
        // when a part contains hundreds of millions of samples.
        uint8_t tail[4];
        retVal = check.seekSet(512 + partBytes - 4) && check.read(tail, sizeof(tail)) == sizeof(tail) &&
            std::memcmp(tail, lastWord, sizeof(tail)) == 0;
    }
    if (check.isOpen())
    {
        retVal = check.close() && retVal;
    }
    Serial.printf("SD readback: session=%lu part=%lu payload=%llu header/size/tail=%s\n",
        static_cast<unsigned long>(session), static_cast<unsigned long>(part),
        static_cast<unsigned long long>(partBytes), retVal ? "OK" : "FAILED");
    return retVal;
}

/**
 * @brief Read filesystem capacity and available bytes; never erase to obtain space.
 * Called only by the disk thread: at startup, before sample generation, and
 * after Stop/deletion. total/free are output references in BYTES. A cluster is the
 * filesystem's allocation unit (a group of sectors); files consume whole clusters.
 * The library counts clusters, so multiply by bytesPerCluster using 64-bit math.
 * A scan failure sets cardState to I/O failure and preserves previous numbers;
 * those retained space values must not be mistaken for a fresh successful scan.
 * Return true only for a successful scan. Never call during active recording.
 */
bool sdRecordingSink::space(uint64_t& total, uint64_t& free)
{
    bool retVal = false;

    // Promote BEFORE multiplication: a 64-GB card cannot fit in a 32-bit byte
    // count. Negative freeClusterCount means a read error; preserve prior values.
    if (mounted)
    {
        const int32_t freeClusters = filesystem.freeClusterCount();
        if (freeClusters >= 0)
        {
            total = static_cast<uint64_t>(filesystem.clusterCount()) * filesystem.bytesPerCluster();
            free = static_cast<uint64_t>(freeClusters) * filesystem.bytesPerCluster();
            retVal = true;
        }
        else
        {
            cardState = recordingStatus::ioFailure;
        }
    }
    else
    {
        total = 0; free = 0;
    }
    return retVal;
}

/**
 * @brief Account for newly occupied file clusters using cached file length only.
 * Called by the disk task after a successful header or payload write. fileSize()
 * reads the library's in-memory size; it does not scan the allocation bitmap.
 * A header rewrite leaves length unchanged. On rollover, partAllocated resets
 * while allocatedBytes retains the old parts, so cluster rounding stays per-file.
 * Preallocation sets the full reserved file length before the first header write,
 * so this accounts for that reservation once. Final free-space scanning after
 * close accounts for an unused tail released by truncation.
 */
void sdRecordingSink::accountGrowth()
{
    const uint64_t occupied = recordingSpace::allocation(file.fileSize(), filesystem.bytesPerCluster());
    if (occupied > partAllocated)
    {
        allocatedBytes += occupied - partAllocated;
        partAllocated = occupied;
    }
}

/**
 * @brief Write, close, reopen and check every byte of a separate 64-KiB test file.
 * Only the optional storage-check build calls this, on the disk task at startup.
 * A new filename is reserved exclusively; existing files are never overwritten.
 * The file remains on the card for inspection. Success verifies this one transfer,
 * not sustained speed or immunity to future power loss. No acquisition is running.
 */
bool sdRecordingSink::verifyStorage()
{
    bool retVal = mount();
    FsFile testFile;
    char path[40] = {};
    uint32_t id = 1;

    // Choose an unused name without assuming an empty card or deleting old tests.
    do
    {
        snprintf(path, sizeof(path), "/sdio_check_%06lu.bin", static_cast<unsigned long>(id++));
    } while (retVal && filesystem.exists(path) && id < 1000000);
    retVal = retVal && testFile.open(&filesystem, path, O_CREAT | O_EXCL | O_RDWR);
    uint8_t block[512];

    // Each byte depends on its absolute position, so misplaced sectors and bytes
    // are detected. write() is the actual file transfer; sync() commits caches.
    for (uint32_t offset = 0; retVal && offset < 65536; offset += sizeof(block))
    {
        for (size_t index = 0; index < sizeof(block); ++index)
        {
            const uint32_t position = offset + index;
            block[index] = static_cast<uint8_t>(position * 37 + (position >> 8) * 11);
        }
        retVal = testFile.write(block, sizeof(block)) == sizeof(block);
    }
    if (retVal)
    {
        retVal = testFile.sync();
    }
    testFile.close();

    // Reopen through the filesystem and check size and all payload bytes, rather
    // than relying only on the recording header/tail check used for large files.
    retVal = retVal && testFile.open(&filesystem, path, O_RDONLY);
    retVal = retVal && testFile.fileSize() == 65536;
    for (uint32_t offset = 0; retVal && offset < 65536; offset += sizeof(block))
    {
        retVal = testFile.read(block, sizeof(block)) == sizeof(block);
        for (size_t index = 0; retVal && index < sizeof(block); ++index)
        {
            const uint32_t position = offset + index;
            retVal = block[index] == static_cast<uint8_t>(position * 37 + (position >> 8) * 11);
        }
    }
    testFile.close();
    Serial.printf("SD full readback: %s bytes=65536 result=%s\n", path, retVal ? "PASS" : "FAIL");
    return retVal;
}
