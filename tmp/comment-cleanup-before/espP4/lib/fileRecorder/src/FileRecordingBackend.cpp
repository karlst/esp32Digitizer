// Copyright (C) 2026 Karl Stock
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file FileRecordingBackend.cpp
 * Write a recording to a list of preallocated SD files. 
 *  - _slots contains that list;
 *  - prepForRecording chooses the files; 
 *  - copyToRingBuffer copies sample bytes into the RAM ring; 
 *  - pump writes queued bytes;
 *  - finishRecording closes the session; 
 *  - verify reads retained data back. The sample-processing task calls copyToRingBuffer and skipInput. 
 * The disk task handles all filesystem calls. 
 * The class treats sample data as bytes and does not control the ADC.
 * 
 * Files are named with a prefix ending with an integer. This class manages file names to avoid collisions
 * A session is one run of the digitizer, from start to stop. Session numbers are stored in file headers.
 * This class assigns the session number
 */
#include "FileRecordingBackend.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

/* **
 * ~FileRecordingBackend - Free the temporary disk-transfer buffer. The caller
 * must stop submitting data and finish all disk operations before destroying
 * this object. The BufferedWriter member frees the ring buffer separately.
 */
FileRecordingBackend::~FileRecordingBackend()
{
    free(_scratchMemory);
}

/* **
 * initializeMemory - Allocate the memory needed for SD writing, before preparing
 * a session. Allocate the 16 KB temporary disk-transfer buffer (_scratchMemory), then
 * ask BufferedWriter to allocate the ring buffer.
 *
 * - allocateRing and releaseRing are caller-supplied functions for allocating and freeing ring memory. 
 * - clockUs measures elapsed microseconds; 
 * - utcSeconds supplies calendar time for file headers. 
 * - Return true only when both buffers are available.
 */
bool FileRecordingBackend::initializeMemory(uint8_t* (*allocateRing)(uint32_t),
    void (*releaseRing)(uint8_t*), uint64_t (*clockUs)(), uint64_t (*utcSeconds)())
{
    bool retVal = !_active && allocateRing && releaseRing && clockUs && utcSeconds;

    if (retVal)
    {
        // PC smoke tests use ordinary Windows memory; P4 needs memory suitable for SD transfers.
#ifdef ESP_PLATFORM
        if (!_scratchMemory)
        {
            _scratchMemory = static_cast<uint8_t*>(heap_caps_aligned_alloc(64, 16384, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
        }
#else
        if (!_scratchMemory)
        {
            _scratchMemory = static_cast<uint8_t*>(malloc(16384));
        }
#endif
        retVal = _scratchMemory && _bufferedWriter.initializeMemory(allocateRing, releaseRing);
        _memoryReady = retVal;
        _clockUs = clockUs;
        _utcSeconds = utcSeconds;
    }

    return retVal;
}

/* **
 * checkOperation - Return true if this filesystem operation succeeded (result == FR_OK).
 * On the first failure, save the operation description in _error and set _failed
 * to true. Later failures do not replace that description.
 *
 * _failed is an atomic flag: the disk task sets it and the task submitting
 * samples may read it. load() reads the flag; store(true) sets it. A later
 * successful operation does not clear it.
 */
bool FileRecordingBackend::checkOperation(FRESULT result, const char* operation)
{
    const bool retVal = result == FR_OK;
    if (!retVal && !_failed.load())
    {
        _error = operation;
        _failed.store(true);
    }

    return retVal;
}

/* **
 * scanHeaders - Read existing recording-file headers before preparing a session.
 * prepareFiles calls this after mounting the card and locating /RECORD. It
 * examines names such as R0000001.BIN and reads the first header bytes of each
 * matching file.
 *
 * - Append files with valid headers and matching sizes to the supplied slots list. 
 * - Set nextFile above the highest existing file number. 
 * - Set nextSession above the highest session number in valid headers.
 *
 * entries counts every directory entry, including unrelated files and subdirectories. 
 * refuse more than 4096 entries to limit the scan and its memory use. 
 *
 * returns True for success, false otherwise. 
 */
bool FileRecordingBackend::scanHeaders(std::vector<Slot>& slots, uint32_t& nextFile, uint64_t& nextSession, uint32_t& entries)
{
    FF_DIR directory = {};
    bool retVal = checkOperation(f_opendir(&directory, _directory.c_str()), "opening recording directory");
    const bool opened = retVal;
    FILINFO info = {};
    bool ended = false;
    entries = 0;


    while (retVal && !ended)
    {
        retVal = checkOperation(f_readdir(&directory, &info), "reading recording directory");
        ended = !retVal || info.fname[0] == '\0';

        if (!ended)
        {
            ++entries;

            if (entries > 4096)
            {
                _error = "recording directory exceeds 4096 entries";
                retVal = false;
            }

            unsigned number = 0;
            bool namedSlot = strlen(info.fname) == 12 && info.fname[0] == 'R' && strcmp(info.fname + 8, ".BIN") == 0;

            for (unsigned iCharacter = 1; namedSlot && iCharacter <= 7; ++iCharacter)
            {
                namedSlot = info.fname[iCharacter] >= '0' && info.fname[iCharacter] <= '9';
                if (namedSlot)
                {
                    number = number * 10 + static_cast<unsigned>(info.fname[iCharacter] - '0');
                }
            }

            if (retVal && namedSlot)
            {
                nextFile = std::max(nextFile, static_cast<uint32_t>(number + 1));

                if (!(info.fattrib & AM_DIR) && info.fsize >= 512)
                {
                    Slot slot;
                    slot._path = _directory + "/" + info.fname;
                    retVal = checkOperation(f_open(&_file, slot._path.c_str(), FA_READ), "opening prepared header");

                    if (retVal)
                    {
                        UINT actual = 0;
                        retVal = checkOperation(f_read(&_file, _scratchMemory, 512, &actual), "reading prepared header") && actual == 512;
                        const bool valid = retVal && slot._sessionFileHeader.decode(_scratchMemory) &&
                                           slot._sessionFileHeader._capacity <= UINT64_MAX - 512 &&
                                           slot._sessionFileHeader._capacity + 512 == info.fsize;
                        const bool closed = checkOperation(f_close(&_file), "closing prepared header");
                        retVal = retVal && closed;

                        if (valid)
                        {
                            if (slot._sessionFileHeader._session == UINT64_MAX)
                            {
                                retVal = false;
                                _error = "session IDs exhausted";
                            }
                            else
                            {
                                nextSession = std::max(nextSession, slot._sessionFileHeader._session + 1);
                                slots.push_back(slot);
                            }
                        }
                    }
                }
            }
        }
    }

    if (opened)
    {
        const bool closed = checkOperation(f_closedir(&directory), "closing recording directory");
        retVal = retVal && closed;
    }

    return retVal;
}

/* **
 * allocate - Create the requested new files, starting with file number first.
 * capacity is the number of sample-data bytes reserved in each file; the
 * 512-byte header is additional.
 *
 * Called during preparation, before samples arrive. f_expand reserves contiguous
 * disk space, then f_write saves a header marking the file unused. FA_CREATE_NEW
 * refuses an existing filename. Return true only after every requested file has
 * been allocated and closed. A failed attempt may leave a new file behind; it
 * does not remove existing files.
 */
bool FileRecordingBackend::allocate(uint32_t first, uint64_t capacity)
{
    bool retVal = first <= 9999999 && _recordingSessionConfig._fileCount <= 10000000 - first;
    for (uint32_t iSlot = 0; retVal && iSlot < _recordingSessionConfig._fileCount; ++iSlot)
    {
        // Report which file is about to be allocated.
        char progress[96];
        snprintf(progress, sizeof(progress), "allocating file %u of %u",
            static_cast<unsigned>(iSlot + 1), static_cast<unsigned>(_recordingSessionConfig._fileCount));
        reportProgress(progress);

        char name[24];
        snprintf(name, sizeof(name), "/R%07u.BIN", static_cast<unsigned>(first + iSlot));
        Slot slot;
        slot._path = _directory + name;
        auto& header = slot._sessionFileHeader;
        header._session = _session;
        header._capacity = capacity;
        header._utcSeconds = _utcSeconds();
        header._sampleRate = _recordingSessionConfig._sampleRate;
        header._bytesPerSample = _recordingSessionConfig._bytesPerSample;
        header._sampleFormat = _recordingSessionConfig._sampleFormat;
        retVal = header.encode(_scratchMemory) && checkOperation(f_open(&_file, slot._path.c_str(), FA_WRITE | FA_CREATE_NEW), "creating new slot");

        if (retVal)
        {
            UINT actual = 0;
            retVal = checkOperation(f_expand(&_file, capacity + 512, 1), "preallocating new slot") &&
                     checkOperation(f_write(&_file, _scratchMemory, 512, &actual), "writing unused header") && actual == 512;
            const bool closed = checkOperation(f_close(&_file), "closing new slot");
            retVal = retVal && closed;
            if (retVal)
            {
                _slots.push_back(slot);
                reportProgress("file allocated and header saved");
            }
        }
    }
    return retVal;
}

/* **
 * prepForRecording - Prepare the selected files and ring buffer for a new recording
 * session. The disk task calls this before any data is submitted. Return true
 * when the first file is open and the backend can accept bytes.
 *
 * config chooses whether to format and allocate, add new files, or use prepared
 * files. Adding files uses only those new files for this session. Reusing
 * existing recordings requires overwriteOldest. This call does not start the
 * ADC.
 */
bool FileRecordingBackend::prepForRecording(RecordingDisk& recordingDisk, const RecordingSessionConfig& config)
{
    return prepareSession(recordingDisk, config, true);
}

/* **
 * preallocate - Reserve new files for later use without starting a recording
 * session. The disk task calls this while no data is being submitted. config may
 * request a fresh disk or additional files; useExisting is refused because it
 * would allocate nothing.
 *
 * Return true when the requested files exist with unused headers. A later
 * prepForRecording call with useExisting can select them for recording.
 */
bool FileRecordingBackend::preallocate(RecordingDisk& recordingDisk, const RecordingSessionConfig& config)
{
    const bool retVal = config._preparation != RecordingSessionConfig::Preparation::useExisting &&
                        prepareSession(recordingDisk, config, false);
    return retVal;
}

/* **
 * resetSession - Clear the previous session's counters and save the new settings.
 * prepareSession calls this only while inactive. Clearing _slots removes the
 * in-memory file list; it does not delete files from the card.
 */
void FileRecordingBackend::resetSession(const RecordingSessionConfig& config)
{
    _failed.store(false);
    _full.store(false);
    _error = "invalid preparation parameters";

    _slots.clear();
    _statistics = {};
    _iSlot = 0;
    _sequence = 0;
    _written = 0;
    _inputPosition = 0;
    _accepted = 0;
    _lostInput = 0;
    _sessionCapacity = 0;
    _session = 1;
    _recordingSessionConfig = config;
}

/* **
 * validateConfig - Check memory, UTC and file-size settings before disk access.
 * Return false if the requested size cannot fit the filesystem's size type.
 * useExisting uses the sizes already stored in file headers, so it does not
 * require a new file count or duration.
 */
bool FileRecordingBackend::validateConfig() const
{
    const auto& config = _recordingSessionConfig;
    const uint64_t rate = uint64_t(config._sampleRate) * config._bytesPerSample;
    bool retVal = _memoryReady && rate && config._bytesPerSample <= _maxSubmission;

    if (retVal)
    {
        const uint64_t utc = _utcSeconds();
        retVal = utc >= 1577836800ULL && utc < 4102444800ULL;
    }

    if (config._preparation != RecordingSessionConfig::Preparation::useExisting)
    {
        retVal = retVal && config._fileCount > 0 && config._fileCount <= 4096 &&
                 config._secondsPerFile && rate <= (std::numeric_limits<FSIZE_t>::max() - 512) / config._secondsPerFile;
    }

    return retVal;
}

/* **
 * prepareDisk - Ask the board adapter to mount the card, formatting only when
 * explicitly requested. Return true when the filesystem is available.
 */
bool FileRecordingBackend::prepareDisk(RecordingDisk& recordingDisk)
{
    _error = "mounting/formatting card";
    reportProgress("mounting card (formatting only if requested)");
    const bool retVal = recordingDisk.prepare(
        _recordingSessionConfig._preparation == RecordingSessionConfig::Preparation::formatAndAllocate);

    if (retVal)
    {
        reportProgress("card mounted");
    }

    return retVal;
}

/* **
 * selectSlots - Select compatible existing files and put them in writing order.
 * Unused files come first. Previously recorded files are eligible only when
 * overwriteOldest is selected; older sessions and sequences come before newer
 * ones. Return false if no eligible file exists. This does not open any file.
 */
bool FileRecordingBackend::selectSlots(const std::vector<Slot>& existing)
{
    const auto& config = _recordingSessionConfig;
    for (const auto& slot : existing)
    {
        const auto& header = slot._sessionFileHeader;
        if (header._sampleRate == config._sampleRate && header._bytesPerSample == config._bytesPerSample &&
            header._sampleFormat == config._sampleFormat &&
            (header._state == 0 || config._fullPolicy == RecordingSessionConfig::FullPolicy::overwriteOldest))
        {
            _slots.push_back(slot);
        }
    }

    // Sort equal session/sequence entries by filename for a repeatable order.
    std::sort(_slots.begin(), _slots.end(), [](const Slot& left, const Slot& right)
    {
        const auto& a = left._sessionFileHeader;
        const auto& b = right._sessionFileHeader;
        if ((a._state == 0) != (b._state == 0))
        {
            return a._state == 0;
        }
        if (a._session != b._session)
        {
            return a._session < b._session;
        }
        if (a._sequence != b._sequence)
        {
            return a._sequence < b._sequence;
        }
        return left._path < right._path;
    });

    const bool retVal = !_slots.empty();
    if (!retVal)
    {
        _error = "no compatible available prepared files";
    }

    return retVal;
}

/* **
 * prepareFiles - Scan the recording directory, then select existing files or
 * allocate new ones according to the session settings. Calculate the selected
 * files' total sample-data capacity. Return false on any required file operation
 * or size check failure. Called after prepareDisk succeeds.
 */
bool FileRecordingBackend::prepareFiles(RecordingDisk& recordingDisk)
{
    const auto& config = _recordingSessionConfig;
    std::vector<Slot> existing;
    uint32_t nextFile = 0;
    uint32_t entries = 0;
    bool retVal = true;

    reportProgress("scanning recording directory");
    _directory = std::string(recordingDisk.getDrive()) + "/RECORD";
    if (config._preparation != RecordingSessionConfig::Preparation::useExisting)
    {
        const FRESULT created = f_mkdir(_directory.c_str());
        retVal = created == FR_EXIST || checkOperation(created, "creating recording directory");
    }

    if (retVal)
    {
        retVal = scanHeaders(existing, nextFile, _session, entries);
    }

    if (retVal)
    {
        reportProgress("directory scan complete");
        if (config._preparation == RecordingSessionConfig::Preparation::useExisting)
        {
            retVal = selectSlots(existing);
        }
        else
        {
            const uint64_t rate = uint64_t(config._sampleRate) * config._bytesPerSample;
            _error = "file allocation/header failed or pool exceeds 4096 entries";
            retVal = config._fileCount <= 4096 - entries && allocate(nextFile, rate * config._secondsPerFile);
        }
    }

    // Include the header's capacity, not the amount written by an earlier run.
    for (const auto& slot : _slots)
    {
        if (UINT64_MAX - _sessionCapacity < slot._sessionFileHeader._capacity)
        {
            retVal = false;
        }
        else
        {
            _sessionCapacity += slot._sessionFileHeader._capacity;
        }
    }

    return retVal;
}

/* **
 * startRecordingSession - Open the first selected file and reset the ring for
 * new data. Return true when copyToRingBuffer may accept samples. This starts
 * the storage session only; it does not start the ADC or create a task.
 */
bool FileRecordingBackend::startRecordingSession()
{
    reportProgress("opening first recording file");
    const bool retVal = openSlot();

    if (retVal)
    {
        _bufferedWriter.start(*this, _clockUs, BufferedWriter::OverflowPolicy::dropIncoming);
        _active = true;
    }

    return retVal;
}

/* **
 * prepareSession - Coordinate preparation for prepForRecording and preallocate.
 * Reset the inactive session, validate settings, prepare the disk and files,
 * then optionally open the first file for recording. Return true only after
 * these steps finish. Keep failures and file cleanup here so every path follows
 * the same rules. No other task may submit samples during preparation.
 */
bool FileRecordingBackend::prepareSession(RecordingDisk& recordingDisk, const RecordingSessionConfig& config, bool startRecording)
{
    if (_active)
    {
        return false;
    }

    resetSession(config);
    bool retVal = validateConfig();

    if (retVal)
    {
        retVal = prepareDisk(recordingDisk);
    }

    if (retVal)
    {
        retVal = prepareFiles(recordingDisk);
    }

    if (retVal && startRecording)
    {
        retVal = startRecordingSession();
    }

    // A failed attempt must not leave an open file or appear ready for samples.
    if (retVal)
    {
        _error = "none";
    }
    else
    {
        _failed.store(true);
        if (_opened)
        {
            f_close(&_file);
            _opened = false;
        }
    }

    return retVal;
}

/* **
 * copyToRingBuffer - Copy incoming bytes into the ring buffer and return how many were
 * accepted. The sample-processing task calls this; it performs no disk access
 * and does not wait for space. length is bytes, not samples, and must contain
 * whole samples.
 *
 * If the ring cannot fit the offered batch, reject that batch and count its
 * bytes as lost. With stopWhenFull, the final submission may be shortened to the
 * remaining file capacity. Bytes beyond that deliberate stopping point are not
 * counted as lost. The caller checks getFull and getFailed to decide when to
 * stop.
 */
uint32_t FileRecordingBackend::copyToRingBuffer(const uint8_t* data, uint32_t length)
{
    uint32_t retVal = 0;
    const bool enabled = _active && !_failed.load() && !_full.load();
    const bool valid = enabled && data && length && length <= _maxSubmission &&
                       length % _recordingSessionConfig._bytesPerSample == 0 && UINT64_MAX - _inputPosition >= length;
    if (enabled && !valid)
    {
        _failed.store(true);
    }

    if (valid)
    {
        uint32_t offered = length;
        if (_recordingSessionConfig._fullPolicy == RecordingSessionConfig::FullPolicy::stopWhenFull)
        {
            offered = static_cast<uint32_t>(std::min<uint64_t>(offered, _sessionCapacity - _accepted));
        }

        if (offered)
        {
            if (_bufferedWriter.submit(data, offered))
            {
                retVal = offered;
                _accepted += offered;
            }
            else
            {
                _lostInput += offered;
            }
        }

        _inputPosition += length;
        if (_recordingSessionConfig._fullPolicy == RecordingSessionConfig::FullPolicy::stopWhenFull && _accepted == _sessionCapacity)
        {
            _full.store(true);
        }
    }
    return retVal;
}

/* **
 * skipInput - Count sample bytes that were lost before they reached copyToRingBuffer. For
 * example, a test generator calls this when it misses a generation deadline.
 *
 * Call from the same task that submits data. bytes must represent whole samples.
 * This updates the session loss count without adding anything to the ring or
 * writing to disk. It records the amount lost, not where each gap occurred.
 * Invalid input sets _failed.
 */
void FileRecordingBackend::skipInput(uint64_t bytes)
{
    if (_active && bytes % _recordingSessionConfig._bytesPerSample == 0 && UINT64_MAX - _inputPosition >= bytes)
    {
        _inputPosition += bytes;
        _lostInput += bytes;
    }

    else
    {
        _failed.store(true);
    }
}

/* **
 * saveHeader - Write the current file header at the beginning of the file. The
 * disk task encodes it into _scratchMemory, seeks to byte zero, writes 512 bytes, then
 * calls f_sync to finish pending filesystem writes. Return true only if all
 * these steps succeed.
 */
bool FileRecordingBackend::saveHeader()
{
    UINT actual = 0;
    const bool retVal = _slots[_iSlot]._sessionFileHeader.encode(_scratchMemory) &&
                        checkOperation(f_lseek(&_file, 0), "seeking header") &&
                        checkOperation(f_write(&_file, _scratchMemory, 512, &actual), "writing header") && actual == 512 &&
                        checkOperation(f_sync(&_file), "synchronizing header");

    return retVal;
}

/* **
 * openSlot - Open the selected preallocated file for writing without changing
 * its size. Set its header to incomplete, record the current session, file
 * sequence and UTC time, then position the file after the 512-byte header.
 *
 * Called by the disk task at session start and when changing files. A sequence
 * number identifies each use of a file, including reuse after wrapping around.
 * Return true when the file and header are ready for sample data.
 */
bool FileRecordingBackend::openSlot()
{
    auto& slot = _slots[_iSlot];
    auto& header = slot._sessionFileHeader;
    header._session = _session;
    header._sequence = _sequence;
    header._state = 1;
    header._validBytes = 0;
    header._streamOffset = _written;
    header._lostBytes = 0;
    header._lossFinal = false;
    header._utcSeconds = _utcSeconds();
    bool retVal = checkOperation(f_open(&_file, slot._path.c_str(), FA_WRITE | FA_READ), "opening file slot");
    _opened = retVal;

    if (retVal)
    {
        retVal = f_size(&_file) == header._capacity + 512 && saveHeader() && checkOperation(f_lseek(&_file, 512), "seeking payload");
    }

    slot._writtenThisSession = retVal;
    return retVal;
}

/* **
 * closeSlot - Mark the current file complete, save its header, and close it. The
 * disk task calls this after successful data writes. Return false if the header
 * cannot be saved or the file cannot be closed. Complete describes the file
 * write; it does not establish that the ADC delivered every sample.
 */
bool FileRecordingBackend::closeSlot()
{
    _slots[_iSlot]._sessionFileHeader._state = 2;
    bool retVal = saveHeader();
    const bool closed = checkOperation(f_close(&_file), "closing file slot");
    _opened = false;
    retVal = retVal && closed;
    return retVal;
}

/* **
 * advance - Finish the full file and open the next file in the selected list.
 * With circular overwrite, moving past the last file returns to the first and
 * replaces its previous recording.
 *
 * Called by write only when the current file has reached its byte capacity.
 * Missing input does not trigger a file change. With stopWhenFull, copyToRingBuffer should
 * already have limited accepted bytes to the total capacity; needing another
 * file here indicates an accounting error. Return false if closing or opening
 * fails, or if the pool is exhausted.
 */
bool FileRecordingBackend::advance()
{
    const uint64_t began = _clockUs();
    bool retVal = closeSlot();

    if (retVal && _iSlot + 1 == _slots.size() &&
        _recordingSessionConfig._fullPolicy == RecordingSessionConfig::FullPolicy::stopWhenFull)
    {
        _full.store(true);
        retVal = false;
        _error = "accepted bytes exceed selected file capacity";
    }

    if (retVal)
    {
        _iSlot = (_iSlot + 1) % static_cast<uint32_t>(_slots.size());
        ++_sequence;
        if (_slots[_iSlot]._writtenThisSession)
        {
            ++_statistics._reuses;
        }
        retVal = openSlot();
        if (retVal)
        {
            ++_statistics._rollovers;
        }
    }

    _statistics._rolloverUs = std::max(_statistics._rolloverUs, _clockUs() - began);
    return retVal;
}

/* **
 * write - Write bytes from the ring buffer to the SD files. BufferedWriter calls
 * this on the disk task and keeps those ring bytes unavailable for reuse until
 * the call returns.
 *
 * Copy up to 16 KB into _scratchMemory with memcpy, then write that chunk with
 * f_write. data + retVal points past the bytes already written. A normal 128 KB
 * batch needs eight such writes; a file boundary or final batch may require
 * smaller chunks.
 *
 * When a file fills, advance opens the next file before writing more bytes.
 * Return the number actually written. A short write or filesystem error sets
 * _failed; the caller must not retry the entire batch and duplicate its
 * already-written bytes.
 */
size_t FileRecordingBackend::write(const uint8_t* data, size_t length)
{
    size_t retVal = 0;
    bool ok = true;
    while (ok && retVal < length)
    {
        if (_slots[_iSlot]._sessionFileHeader._validBytes == _slots[_iSlot]._sessionFileHeader._capacity)
        {
            ok = advance();
        }

        if (ok)
        {
            auto& header = _slots[_iSlot]._sessionFileHeader;
            const UINT amount = static_cast<UINT>(std::min<uint64_t>(
                std::min<size_t>(length - retVal, 16384), header._capacity - header._validBytes));
            memcpy(_scratchMemory, data + retVal, amount);
            UINT actual = 0;
            const uint64_t began = _clockUs();
            ok = checkOperation(f_write(&_file, _scratchMemory, amount, &actual), "writing payload") && actual == amount;
            _statistics._writeUs = std::max(_statistics._writeUs, _clockUs() - began);
            retVal += actual;
            _written += actual;
            header._validBytes += actual;
        }
    }

    if (!ok)
    {
        if (strcmp(_error, "none") == 0)
        {
            _error = "short payload write";
        }
        _failed.store(true);
    }

    return retVal;
}

/* **
 * finalizeLossTotals - Save the final session loss count in every file still
 * retained from this run. The disk task calls this after sample submission has
 * stopped and the ring has been drained.
 *
 * Each header stores the same session-wide total, not that file's share. Do not
 * add the header counts together. Set lossFinal only when this final update is
 * being made. Return false if a header update fails. The original file timestamp
 * stays unchanged.
 */
bool FileRecordingBackend::finalizeLossTotals()
{
    bool retVal = true;
    const uint32_t iSlotLast = _iSlot;
    for (uint32_t iSlot = 0; retVal && iSlot < _slots.size(); ++iSlot)
    {
        auto& slot = _slots[iSlot];

        if (slot._writtenThisSession)
        {
            _iSlot = iSlot;
            slot._sessionFileHeader._lostBytes = _lostInput;
            slot._sessionFileHeader._lossFinal = true;
            retVal = checkOperation(f_open(&_file, slot._path.c_str(), FA_WRITE), "opening final loss header");
            if (retVal)
            {
                retVal = saveHeader();
                const bool closed = checkOperation(f_close(&_file), "closing final loss header");
                retVal = retVal && closed;
            }
        }
    }

    _iSlot = iSlotLast;

    return retVal;
}

/* **
 * flush - Ask the filesystem to finish pending writes for the current file. The
 * disk task calls this after submissions stop. Return true if no file is open or
 * f_sync succeeds. This does not read the data back for verification.
 */
bool FileRecordingBackend::flush()
{
    return !_opened || checkOperation(f_sync(&_file), "flushing payload");
}

/* **
 * pump - Let BufferedWriter write one batch of queued data on the disk task.
 * During recording, drain is false so the writer can wait for a full batch.
 * After submissions stop, drain may be true to write a smaller final batch.
 * Return false and set _failed if the backend is inactive or the write fails.
 */
bool FileRecordingBackend::pump(bool drain)
{
    const bool retVal = _active && !_failed.load() && _bufferedWriter.pump(drain);
    if (!retVal)
    {
        _failed.store(true);
    }
    return retVal;
}

/* **
 * finishRecording - Finish a recording after the caller has stopped submitting samples.
 * Write the remaining buffered bytes, finalize and close the last file, then
 * save session loss totals in the retained headers.
 *
 * Return true if those storage operations succeed. This does not mean no samples
 * were lost; the caller must also check the loss and acquisition counters. On
 * failure, close any open file without retrying a partially written batch.
 */
bool FileRecordingBackend::finishRecording()
{
    bool retVal = _active;
    if (_active)
    {
        const bool drained = _bufferedWriter.finish();
        _statistics._lostBytes = _lostInput;
        retVal = drained && !_failed.load();

        if (retVal && _opened)
        {
            retVal = closeSlot();
        }

        if (_opened)
        {
            f_close(&_file);
            _opened = false;
        }

        if (retVal)
        {
            retVal = finalizeLossTotals();
        }
        _active = false;
    }
    if (!retVal)
    {
        if (strcmp(_error, "none") == 0)
        {
            _error = "finish failed: invalid input or incomplete header";
        }

        _failed.store(true);
    }
    return retVal;
}

/* **
 * verify - Read back the files retained from this session after finishRecording. Compare
 * each 512-byte header with the expected header and, if checkPayload is
 * supplied, pass it the sample bytes in their original recording order.
 *
 * The callback receives the bytes, their count and their original byte offset
 * within the session. The overload with context also passes the caller's context
 * pointer. The data buffer is reused after each callback, so the callback must
 * copy anything it needs to keep. Return false if a file check or callback
 * fails.
 *
 * Earlier overwritten data no longer exists and cannot be verified. Files
 * untouched by this session are not checked here. Without a payload callback,
 * this reads the sample bytes but does not establish that their contents are
 * correct.
 * 
 * First signature is used by chokeTest. Second signature used by SdRecorder
 */
bool FileRecordingBackend::verify(bool (*checkPayload)(const uint8_t*, uint32_t, uint64_t))
{
    // The legacy callback is borrowed only for this synchronous readback call.
    return verify([](void* context, const uint8_t* data, uint32_t bytes, uint64_t offset)
    {
        const auto callback = *static_cast<bool (**)(const uint8_t*, uint32_t, uint64_t)>(context);
        return !callback || callback(data, bytes, offset);
    }, &checkPayload);
}

/* **
 * verify - Perform the readback described above, passing context to the checking
 * function with each chunk. SdRecorder passes its own address so the checker
 * can update that recorder's checksums and waveform summary.
 */
bool FileRecordingBackend::verify(bool (*checkPayload)(void*, const uint8_t*, uint32_t, uint64_t), void* context)
{
    bool retVal = !_active && !_failed.load();
    std::vector<const Slot*> ordered;
    for (const auto& slot : _slots)
    {
        if (slot._writtenThisSession)
        {
            ordered.push_back(&slot);
        }
    }

    std::sort(ordered.begin(), ordered.end(), [](const Slot* left, const Slot* right)
    {
        return left->_sessionFileHeader._streamOffset < right->_sessionFileHeader._streamOffset;
    });

    for (const auto* selected : ordered)
    {
        const auto& slot = *selected;
        if (retVal && slot._writtenThisSession)
        {
            retVal = checkOperation(f_open(&_file, slot._path.c_str(), FA_READ), "opening readback file");
            if (retVal)
            {
                UINT actual = 0;
                uint8_t expected[512];
                retVal = f_size(&_file) == slot._sessionFileHeader._capacity + 512 &&
                         slot._sessionFileHeader.encode(expected) &&
                         checkOperation(f_read(&_file, _scratchMemory, 512, &actual), "reading header") && actual == 512 &&
                         memcmp(_scratchMemory, expected, 512) == 0;
                uint64_t offset = 0;
                while (retVal && offset < slot._sessionFileHeader._validBytes)
                {
                    const UINT amount = static_cast<UINT>(std::min<uint64_t>(16384, slot._sessionFileHeader._validBytes - offset));
                    retVal = checkOperation(f_read(&_file, _scratchMemory, amount, &actual), "reading payload") && actual == amount;
                    if (retVal && checkPayload)
                    {
                        retVal = checkPayload(context, _scratchMemory, actual, slot._sessionFileHeader._streamOffset + offset);
                    }
                    offset += actual;
                }
                const bool closed = checkOperation(f_close(&_file), "closing readback");
                retVal = retVal && closed;
            }
        }
    }

    if (!retVal)
    {
        _error = "session readback mismatch or I/O error";
    }

    return retVal;
}

/* **
 * getFailed - Return true if the backend or ring-buffer writer has recorded a
 * failure. The sample-submission task may read this between submissions; it does
 * not need to read disk-task statistics.
 */
bool FileRecordingBackend::getFailed() const
{
    return _failed.load() || _bufferedWriter.getFailed();
}

/* **
 * getFull - Return true when the selected stopWhenFull file capacity has been
 * accepted. This is file-pool exhaustion, not a full RAM ring buffer. Some
 * accepted bytes may still be waiting to be written.
 */
bool FileRecordingBackend::getFull() const
{
    return _full.load();
}

/* **
 * getQueuedBytes - Return the number of bytes currently waiting in the ring
 * buffer, including bytes whose write has not yet completed.
 */
uint32_t FileRecordingBackend::getQueuedBytes() const
{
    return _bufferedWriter.getQueuedBytes();
}

/* **
 * getStatistics - Return the current file counters together with BufferedWriter
 * statistics. Call from the disk task; another task needs a safely shared copy
 * rather than reading these changing fields directly.
 */
FileRecordingBackend::Statistics FileRecordingBackend::getStatistics()
{
    Statistics retVal = _statistics;
    retVal._session = _session;
    retVal._written = _written;
    retVal._fileNumber = _iSlot + 1;
    retVal._files = static_cast<uint32_t>(_slots.size());
    retVal._buffer = _bufferedWriter.getStatistics();
    return retVal;
}

/* **
 * getFileCapacities - Return the sample-data capacity of each selected file, in
 * file-pool order. The disk task calls this after preparation to configure
 * retained-file checksums. Header bytes are not included.
 */
std::vector<uint64_t> FileRecordingBackend::getFileCapacities() const
{
    std::vector<uint64_t> retVal;
    for (const auto& slot : _slots)
    {
        retVal.push_back(slot._sessionFileHeader._capacity);
    }

    return retVal;
}

/* **
 * getError - Return the stored error description. The text belongs to this
 * object; preparation can replace it for the next session.
 */
const char* FileRecordingBackend::getError() const
{
    return _error;
}
