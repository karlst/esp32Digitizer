/**
 * @file sdRecordingSink.cpp
 * @brief Keep card-specific details out of the reusable RAM buffer/writer.
 * Reading path: mount(), open(), write(), close(). Each part begins with a
 * 512-byte header; data after it is described by that header, not this filesystem.
 * An open/incomplete header remains distinguishable after power loss or failure.
 */
#include "sdRecordingSink.h"
#include <cstring>
#include <cstdio>

/** @brief Put an unsigned integer in the header, least significant byte first. */
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
 */
bool sdRecordingSink::mount()
{
    if (!busStarted)
    {
        bus.begin(5, 6, 7, 4); // SCLK, MISO (DO), MOSI (DI), chip select.
        busStarted = true;
    }
    if (!mounted)
    {
        // This controller belongs solely to the card. USER_SPI_BEGIN keeps our
        // explicit pin assignments rather than letting the library select defaults.
        // begin() detects FAT/exFAT; no format function is ever called.
        mounted = filesystem.begin(SdSpiConfig(4, DEDICATED_SPI | USER_SPI_BEGIN, SD_SCK_MHZ(10), &bus));
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
 */
bool sdRecordingSink::recordingName(const char* name, uint32_t& foundSession, uint32_t& foundPart)
{
    unsigned long sessionValue = 0, partValue = 0;
    int end = 0;
    bool retVal = std::sscanf(name, "recording_%lu_%lu.bin%n", &sessionValue, &partValue, &end) == 2 &&
        name[end] == '\0' && sessionValue && partValue;
    if (retVal)
    {
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
 */
bool sdRecordingSink::open(uint32_t sampleRate)
{
    bool retVal = !file.isOpen() && mount();
    if (retVal && !filesystem.exists("/recordings"))
    {
        retVal = filesystem.mkdir("/recordings");
    }
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
    if (retVal && highest != UINT32_MAX)
    {
        session = highest + 1; part = 1; rate = sampleRate;
        payloadBytes = 0; headerBytes = 0; partStart = 0;
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
 * Completed writes count toward bytesWritten even when rewriting this header.
 */
bool sdRecordingSink::header(uint32_t disposition)
{
    uint8_t bytes[512] = {};
    std::memcpy(bytes, "S3REC001", 8);
    putLittle(bytes + 8, 512, 4); putLittle(bytes + 12, 1, 4);
    putLittle(bytes + 16, 1, 4); putLittle(bytes + 20, rate, 4);
    putLittle(bytes + 24, 4, 4); putLittle(bytes + 28, disposition, 4);
    putLittle(bytes + 32, partBytes, 8); putLittle(bytes + 40, partStart / 4, 8);
    putLittle(bytes + 48, session, 4); putLittle(bytes + 52, part, 4);
    bool retVal = file.seekSet(0);
    if (retVal)
    {
        const size_t written = file.write(bytes, sizeof(bytes));
        if (written > 0)
        {
            headerBytes += written;
        }
        retVal = written == sizeof(bytes);
    }
    if (retVal)
    {
        retVal = file.seekSet(512 + partBytes);
    }
    return retVal;
}

/** @brief Exclusively create one part and synchronize its initial open header. */
bool sdRecordingSink::openPart()
{
    char path[96];
    std::snprintf(path, sizeof(path), "/recordings/recording_%06lu_%03lu.bin",
        static_cast<unsigned long>(session), static_cast<unsigned long>(part));
    const bool opened = file.open(&filesystem, path, O_CREAT | O_EXCL | O_RDWR);
    partBytes = 0;
    bool retVal = opened && header(0) && file.sync();
    if (!retVal && file.isOpen())
    {
        file.close();
    }
    return retVal;
}

/**
 * @brief Append bytes, opening a new part before the agreed size limit.
 * Returns only payload bytes actually accepted. Rollover finishes the previous
 * part first; if that fails, stop instead of hiding a possibly incomplete file.
 */
size_t sdRecordingSink::write(const uint8_t* data, size_t length)
{
    size_t retVal = 0;
    bool writable = file.isOpen();
    if (writable && 512 + partBytes + length >= partLimit)
    {
        writable = closePart(true) && part != UINT32_MAX;
        if (writable)
        {
            ++part; partStart = payloadBytes;
            writable = openPart();
        }
    }
    if (writable)
    {
        const size_t written = file.write(data, length);
        if (written > 0)
        {
            retVal = static_cast<size_t>(written);
            partBytes += retVal; payloadBytes += retVal;
            if (written >= sizeof(lastWord))
            {
                std::memcpy(lastWord, data + written - sizeof(lastWord), sizeof(lastWord));
            }
        }
    }
    return retVal;
}

/** @brief Ask the filesystem to synchronize pending data; failure is observable. */
bool sdRecordingSink::flush()
{
    const bool retVal = file.isOpen() && file.sync();
    return retVal;
}

/**
 * @brief Finalize a part; do not mark complete unless its earlier data synchronized.
 * A sudden power loss can still defeat the card's internal cache. These markers
 * describe successful software calls, not a promise against unplugging power.
 */
bool sdRecordingSink::closePart(bool complete)
{
    bool retVal = file.isOpen();
    if (file.isOpen())
    {
        const bool dataSaved = flush();
        retVal = header(complete && dataSaved ? 1 : 2) && dataSaved;
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

/** @brief Close the final part, preserving incomplete status after an ADC/storage fault. */
bool sdRecordingSink::close(bool complete)
{
    const bool retVal = !file.isOpen() || closePart(complete);
    return retVal;
}

/**
 * @brief Delete only our canonical recording files, and only while no file is open.
 * Called solely for an explicit delete command after acquisition has stopped.
 * Unrelated files and subdirectories are preserved; there is no format operation.
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
 */
bool sdRecordingSink::verifyPart(uint32_t disposition)
{
    char path[96];
    std::snprintf(path, sizeof(path), "/recordings/recording_%06lu_%03lu.bin",
        static_cast<unsigned long>(session), static_cast<unsigned long>(part));
    FsFile check;
    uint8_t actual[56] = {}, expected[56] = {};
    std::memcpy(expected, "S3REC001", 8);
    putLittle(expected + 8, 512, 4); putLittle(expected + 12, 1, 4);
    putLittle(expected + 16, 1, 4); putLittle(expected + 20, rate, 4);
    putLittle(expected + 24, 4, 4); putLittle(expected + 28, disposition, 4);
    putLittle(expected + 32, partBytes, 8); putLittle(expected + 40, partStart / 4, 8);
    putLittle(expected + 48, session, 4); putLittle(expected + 52, part, 4);
    bool retVal = check.open(&filesystem, path, O_RDONLY) && check.fileSize() == 512 + partBytes &&
        check.read(actual, sizeof(actual)) == sizeof(actual) &&
        std::memcmp(actual, expected, sizeof(actual)) == 0;
    if (retVal && disposition == 1 && partBytes >= 4)
    {
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

/** @brief Read filesystem capacity and available bytes; never erase to obtain space. */
void sdRecordingSink::space(uint64_t& total, uint64_t& free)
{
    // Promote BEFORE multiplication: a 64-GB card cannot fit in a 32-bit byte
    // count. Negative freeClusterCount means a read error; preserve prior values.
    if (mounted)
    {
        const int32_t freeClusters = filesystem.freeClusterCount();
        if (freeClusters >= 0)
        {
            total = static_cast<uint64_t>(filesystem.clusterCount()) * filesystem.bytesPerCluster();
            free = static_cast<uint64_t>(freeClusters) * filesystem.bytesPerCluster();
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
}
