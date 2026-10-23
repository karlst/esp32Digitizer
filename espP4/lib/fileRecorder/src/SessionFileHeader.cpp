// Copyright (C) 2026 Karl Stock
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file SessionFileHeader.cpp
 * Encode/decode the stable header prefix and session metadata with a checksum.
 * Bytes 0–50 contain version, length, UTC text and Karl's message. Metadata begins
 * at 64; bytes 508–511 hold CRC32 of bytes 0–507. All integers are little-endian.
 */
#include "SessionFileHeader.h"
#include <cstring>
#include <ctime>

/* **
 * put - Store value in the requested number of bytes, lowest byte first. For
 * example, 0x1234 becomes bytes 34 12. This avoids depending on the processor's
 * integer layout.
 */
static void put(uint8_t* data, uint64_t value, unsigned bytes)
{
    for (unsigned iByte = 0; iByte < bytes; ++iByte)
    {
        data[iByte] = static_cast<uint8_t>(value >> (8 * iByte));
    }
}

/* **
 * get - Read an integer stored lowest byte first. Assemble it one byte at a time
 * so data need not be aligned to a processor word boundary.
 */
static uint64_t get(const uint8_t* data, unsigned bytes)
{
    uint64_t retVal = 0;
    for (unsigned iByte = 0; iByte < bytes; ++iByte)
    {
        retVal |= uint64_t(data[iByte]) << (8 * iByte);
    }
    return retVal;
}

/* **
 * checksum - Calculate the CRC32 checksum of length bytes. The header stores
 * this value so later reads can detect changed header bytes. This implementation
 * also runs in the PC tests.
 */
uint32_t SessionFileHeader::checksum(const uint8_t* data, uint32_t length)
{
    uint32_t retVal = 0xffffffffu;
    for (uint32_t iByte = 0; iByte < length; ++iByte)
    {
        retVal ^= data[iByte];
        for (unsigned bit = 0; bit < 8; ++bit)
        {
            retVal = (retVal >> 1) ^ (0xedb88320u & (0u - (retVal & 1u)));
        }
    }
    return ~retVal;
}

/* **
 * encode - Build the complete 512-byte file header in data. The caller must
 * supply a buffer at least that large. Return false for invalid time, capacity
 * or sample settings.
 *
 * State 0 means unused, state 1 means recording was started but not completed,
 * and state 2 means file writing completed. The header contains a checksum of
 * its own bytes; it does not contain a checksum of the recorded samples.
 */
bool SessionFileHeader::encode(uint8_t* data) const
{
    bool retVal = _session != 0 && _capacity != 0 && _validBytes <= _capacity &&
                  _bytesPerSample != 0 && _sampleRate != 0 && _capacity % _bytesPerSample == 0 &&
                  _validBytes % _bytesPerSample == 0 && _streamOffset % _bytesPerSample == 0 &&
                  _state <= 2 && (_state != 0 || _validBytes == 0) &&
                  _utcSeconds >= 1577836800ULL && _utcSeconds < 4102444800ULL;
    if (retVal)
    {
        const time_t seconds = static_cast<time_t>(_utcSeconds);
        tm utc = {};
#ifdef _WIN32
        retVal = gmtime_s(&utc, &seconds) == 0;
#else
        retVal = gmtime_r(&seconds, &utc) != nullptr;
#endif
        char timestamp[21] = {};
        retVal = retVal && strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc) == 20;
        if (retVal)
        {
            memset(data, 0, 512);
            data[0] = 1;
            put(data + 1, 512, 2);
            memcpy(data + 3, timestamp, 20);
            memcpy(data + 23, "Larry Lytle loves Lightning!", 28);
            memcpy(data + 64, "RINGSD02", 8);
            put(data + 72, _session, 8);
            put(data + 80, _sequence, 8);
            put(data + 88, _capacity, 8);
            put(data + 96, _validBytes, 8);
            put(data + 104, _streamOffset, 8); // 112-119 remain reserved/zero.
            put(data + 120, _utcSeconds, 8);
            put(data + 128, _sampleRate, 4);
            put(data + 132, _bytesPerSample, 4);
            put(data + 136, _sampleFormat, 4);
            put(data + 140, _state, 4);
            put(data + 144, _lostBytes, 8);
            put(data + 152, _lossFinal ? 1 : 0, 4);
            put(data + 508, checksum(data, 508), 4);
        }
    }
    return retVal;
}

/* **
 * decode - Read and validate a 512-byte header from data. Check its format
 * identifier, version, length, checksum and stored values before accepting it.
 * Return true for a supported, consistent header. This does not read or validate
 * the sample data following it.
 */
bool SessionFileHeader::decode(const uint8_t* data)
{
    bool retVal = data[0] == 1 && get(data + 1, 2) == 512 &&
                  memcmp(data + 64, "RINGSD02", 8) == 0 && get(data + 508, 4) == checksum(data, 508);
    if (retVal)
    {
        _session = get(data + 72, 8);
        _sequence = get(data + 80, 8);
        _capacity = get(data + 88, 8);
        _validBytes = get(data + 96, 8);
        _streamOffset = get(data + 104, 8);
        _utcSeconds = get(data + 120, 8);
        _sampleRate = static_cast<uint32_t>(get(data + 128, 4));
        _bytesPerSample = static_cast<uint32_t>(get(data + 132, 4));
        _sampleFormat = static_cast<uint32_t>(get(data + 136, 4));
        _state = static_cast<uint32_t>(get(data + 140, 4));
        _lostBytes = get(data + 144, 8);
        _lossFinal = get(data + 152, 4) == 1;
        uint8_t expected[512];
        retVal = encode(expected) && memcmp(expected, data, sizeof(expected)) == 0;
    }
    return retVal;
}
