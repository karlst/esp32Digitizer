// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Serialize complete intervals into [header][tBlock][sBlock]...[zero marker].
 * Reading path: start supplies the UTC origin/configuration; beginBlock submits
 * the interval metadata; submitSamples copies its three-byte samples in batches;
 * closeFile rotates after the configured pair count; finish closes a short file.
 * The P4 formatting task is the single caller while acquisition is active.
 * TimedFileSink is the submission interface: the P4 implementation copies bytes
 * into a separate ring and queues file openings/closures for the disk task.
 * Successful submission is not confirmation that SD writing has finished.
 */
#include "TimedRecordingStream.h"
#include <limits>

/* **
 * TimedRecordingStream - Borrow a sink that outlives this single-task formatter.
 */
TimedRecordingStream::TimedRecordingStream(TimedFileSink& timedFileSink)
    : _timedFileSink(timedFileSink)
{
}

/* **
 * start - Configure an idle formatter. utcFirst identifies the first retained
 * PPS, not the ADC startup time. No header or empty file is emitted yet. The
 * caller must prepare the requested file pool separately before feeding data.
 */
bool TimedRecordingStream::start(uint64_t utcFirst, const TimedRecordingConfig& config)
{
    const bool retVal = !_active && config._pairsPerFile > 0 && config._pairsPerFile <= 65535 &&
        config._fileCount > 0 && config._fileCount <= 4096 && config._delayNs >= 0;
    if (retVal)
    {
        _config = config;
        _utcFirst = utcFirst;
        _intervals = _tcPpsLast = _tcFsLast = 0;
        _files = _pairs = _remaining = 0;
        _opened = _failed = _full = false;
        _active = true;
    }
    return retVal;
}

/* **
 * beginBlock - Accept complete interval metadata before accepting its samples.
 * The caller supplies file-relative nSec; rejecting mismatches catches missing
 * or duplicate intervals instead of silently repairing their time. Adjacent
 * records share the same closing/opening PPS count, including across files.
 */
bool TimedRecordingStream::beginBlock(const TimeBlock& timeBlock)
{
    bool retVal = _active && !_failed && !_full && !_remaining &&
        TimedFileFormat::validBlock(timeBlock) && timeBlock._nSec == _pairs &&
        (!_intervals || (timeBlock._tcPpsFirst == _tcPpsLast && timeBlock._tcFsFirst > _tcFsLast)) &&
        _intervals < std::numeric_limits<uint64_t>::max() - _utcFirst;
    if (retVal && !_opened)
    {
        // Start a file only when its first complete interval is available.
        // _intervals counts all previous intervals; adding it to the acquisition
        // UTC origin gives this file's opening PPS. File-relative nSec is zero.
        uint8_t bytes[TimedFileFormat::_headerBytes];
        TimedFileHeader header;
        header._utcFirst = _utcFirst + _intervals;
        header._delayNs = _config._delayNs;
        header._pairsPerFile = _config._pairsPerFile;
        // Name the file for its first saved analog sample, not its opening PPS.
        // Integer nanoseconds are sufficient for flooring to a whole second:
        // subtracting an integer delay after flooring gives the same second.
        // Reject multiplication overflow and dates before the Unix epoch rather
        // than wrapping an unsigned UTC value into a plausible filename.
        const uint64_t deltaTicks = timeBlock._tcFsFirst - timeBlock._tcPpsFirst;
        const uint64_t ppsTicks = timeBlock._tcPpsLast - timeBlock._tcPpsFirst;
        const uint64_t billion = 1000000000;
        retVal = deltaTicks <= std::numeric_limits<uint64_t>::max() / billion;
        uint64_t utcSample = header._utcFirst;
        if (retVal)
        {
            const int64_t offsetNs = int64_t(deltaTicks * billion / ppsTicks) - header._delayNs;
            if (offsetNs < 0)
            {
                const uint64_t secondsBack = uint64_t(-offsetNs + billion - 1) / billion;
                retVal = utcSample >= secondsBack;
                if (retVal)
                {
                    utcSample -= secondsBack;
                }
            }
        }
        retVal = retVal && TimedFileFormat::encodeHeader(header, bytes, sizeof(bytes)) &&
            _timedFileSink.beginFile(_files % _config._fileCount, utcSample) &&
            _timedFileSink.submit(bytes, sizeof(bytes));
        _opened = retVal;
        if (retVal)
        {
            ++_files;
        }
    }
    if (retVal)
    {
        // Submit metadata before samples. _remaining prevents the next tBlock
        // from being accepted until exactly sCount samples have been copied.
        uint8_t bytes[TimedFileFormat::_tBlockBytes];
        retVal = TimedFileFormat::encodeBlock(timeBlock, bytes, sizeof(bytes)) &&
            _timedFileSink.submit(bytes, sizeof(bytes));
        if (retVal)
        {
            _remaining = timeBlock._sCount;
            _tcPpsLast = timeBlock._tcPpsLast;
            _tcFsLast = timeBlock._tcFsLast;
        }
    }
    _failed = _failed || !retVal;
    return retVal;
}

/* **
 * submitSamples - Copy packed three-byte values through the sink. count is a
 * sample count, not bytes. Batches may be shorter than an sBlock; a batch may
 * not cross its end. Close a full file only after its last sample was accepted.
 * No partial acceptance is retried, and failed recordings receive no end marker.
 */
bool TimedRecordingStream::submitSamples(const uint8_t* packed, uint32_t count)
{
    bool retVal = _active && !_failed && _opened && packed && count && count <= _remaining &&
        count <= std::numeric_limits<size_t>::max() / 3;
    if (retVal)
    {
        retVal = _timedFileSink.submit(packed, size_t(count) * 3);
    }
    if (retVal)
    {
        _remaining -= count;
        if (!_remaining)
        {
            ++_pairs;
            ++_intervals;
            if (_pairs == _config._pairsPerFile)
            {
                retVal = closeFile();
            }
        }
    }
    _failed = _failed || !retVal;
    return retVal;
}

/* **
 * closeFile - Append the marker at a tBlock position and queue closure. This
 * changes neither headers nor previously submitted bytes. The disk adapter
 * reports actual completion later. Called only with complete sample blocks.
 */
bool TimedRecordingStream::closeFile()
{
    uint8_t bytes[TimedFileFormat::_tBlockBytes];
    TimedFileFormat::encodeEnd(bytes);
    const bool retVal = _timedFileSink.submit(bytes, sizeof(bytes)) && _timedFileSink.endFile();
    if (retVal)
    {
        _opened = false;
        _pairs = 0;
        _full = !_config._overwrite && _files == _config._fileCount;
    }
    return retVal;
}

/* **
 * finish - End after a complete PPS interval. An unfinished sBlock is a fault,
 * not a normal shorter block. Empty sessions create no files. Return means
 * requests were accepted; it does not certify their writes or card flush.
 */
bool TimedRecordingStream::finish()
{
    bool retVal = _active && !_failed && !_remaining;
    if (retVal && _opened)
    {
        retVal = closeFile();
    }
    _failed = _failed || !retVal;
    _active = false;
    return retVal;
}

/* **
 * getFailed - Report a latched metadata, byte-count or sink-acceptance failure.
 */
bool TimedRecordingStream::getFailed() const
{
    return _failed;
}

/* **
 * getFull - True after closure of the final allocated file has been queued in
 * stop-on-full mode. It does not confirm that disk writes or flushes completed.
 */
bool TimedRecordingStream::getFull() const
{
    return _full;
}
