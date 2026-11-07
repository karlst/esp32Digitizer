// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// Read encodeHeader/encodeBlock for offsets; putInteger defines byte order.
#include "TimedFileFormat.h"
#include <cstring>

/* **
 * putInteger - Store the low bytes of an integer in little-endian order.
 * Callers supply a validated buffer and a width of 2, 4 or 8 bytes. Explicit
 * shifts avoid unaligned stores and differences between PC and P4 struct layout.
 */
static void putInteger(uint8_t* data, uint64_t value, unsigned bytes)
{
    for (unsigned iByte = 0; iByte < bytes; ++iByte)
    {
        data[iByte] = static_cast<uint8_t>(value >> (8 * iByte));
    }
}

/* **
 * encodeHeader - Validate metadata and write all 256 bytes once at file start.
 * Offsets: DG at 0, version at 2, UTC origin at 4, correction at 12, pair limit
 * at 16, and 236 zero reserved bytes at 20. The header is never rewritten with
 * completion counters. UTC zero is permitted for synthetic relative-time tests.
 */
bool TimedFileFormat::encodeHeader(const TimedFileHeader& header, uint8_t* data, size_t capacity)
{
    const bool retVal = data && capacity >= _headerBytes && header._pairsPerFile > 0 &&
        header._pairsPerFile <= 65535 && header._delayNs >= 0;
    if (retVal)
    {
        std::memset(data, 0, _headerBytes);
        data[0] = 'D';
        data[1] = 'G';
        putInteger(data + 2, _version, 2);
        putInteger(data + 4, header._utcFirst, 8);
        putInteger(data + 12, static_cast<uint32_t>(header._delayNs), 4);
        putInteger(data + 16, header._pairsPerFile, 4);
    }
    return retVal;
}

/* **
 * validBlock - Require enough samples to interpolate between distinct measured
 * endpoints and ordered PPS ticks. Block boundaries occur shortly AFTER each
 * PPS, so the last sample may follow the closing PPS. The first sample must
 * follow the opening PPS and precede the next PPS. PPS validity/association is the caller's
 * responsibility; increasing counts alone do not establish a valid GPS signal.
 */
bool TimedFileFormat::validBlock(const TimeBlock& timeBlock)
{
    const bool retVal = timeBlock._sCount >= 2 &&
        timeBlock._tcPpsFirst < timeBlock._tcPpsLast &&
        timeBlock._tcFsFirst >= timeBlock._tcPpsFirst &&
        timeBlock._tcFsFirst < timeBlock._tcFsLast &&
        timeBlock._tcFsFirst < timeBlock._tcPpsLast;
    return retVal;
}

/* **
 * encodeBlock - Write four 64-bit tick counts, sample count, second number and
 * ten reserved zero bytes. sCount determines exactly how many sample bytes a
 * reader skips before attempting to read another tBlock; sample contents are
 * never searched for a special marker value.
 */
bool TimedFileFormat::encodeBlock(const TimeBlock& timeBlock, uint8_t* data, size_t capacity)
{
    const bool retVal = data && capacity >= _tBlockBytes && validBlock(timeBlock);
    if (retVal)
    {
        std::memset(data, 0, _tBlockBytes);
        putInteger(data, timeBlock._tcFsFirst, 8);
        putInteger(data + 8, timeBlock._tcFsLast, 8);
        putInteger(data + 16, timeBlock._tcPpsFirst, 8);
        putInteger(data + 24, timeBlock._tcPpsLast, 8);
        putInteger(data + 32, timeBlock._sCount, 4);
        putInteger(data + 36, timeBlock._nSec, 2);
    }
    return retVal;
}

/* **
 * encodeEnd - Fill a caller-provided 48-byte buffer with the normal end marker.
 * Only a tBlock position is interpreted as an end marker; zero sample values
 * remain ordinary samples. The caller guarantees the output pointer is valid.
 */
void TimedFileFormat::encodeEnd(uint8_t* data)
{
    std::memset(data, 0, _tBlockBytes);
}
