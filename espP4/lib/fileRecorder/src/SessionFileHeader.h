// Copyright (C) 2026 Karl Stock
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file SessionFileHeader.h
 * Store recording information in a fixed 512-byte header with a defined byte layout.
 */
#pragma once
#include <cstdint>

/**
 * Information stored in each file's 512-byte header. No separate disk catalog is
 * required. _session identifies one run; _sequence gives the writing order of
 * files within that run, including replacements when files are reused.
 * _capacity is the reserved sample-data space and _validBytes is how much of
 * it was written; neither includes the 512-byte header. Unused space at the end
 * of a partially filled file is not recorded data.
 *
 * _streamOffset counts saved sample bytes from the start of the
 * session; it is not elapsed time if samples were lost.
 *
 * Each retained header stores the session loss total in _lostBytes. _lossFinal
 * says that sample submission has stopped and this total is final. The locations
 * of individual gaps are not stored.
 *
 * _state is 0 for allocated but unused, 1 for started but incomplete, or 2 for
 * completed file writing. UTC records when the file was opened, to the nearest
 * second; unused files initially hold their allocation time. The CRC checksum
 * detects damaged header bytes. It does not guarantee that a sudden power loss
 * leaves a complete file.
 */
struct SessionFileHeader
{
    uint64_t _session = 0, _sequence = 0, _capacity = 0, _validBytes = 0;
    uint64_t _streamOffset = 0, _utcSeconds = 0, _lostBytes = 0;
    bool _lossFinal = false;
    uint32_t _sampleRate = 0, _bytesPerSample = 0, _sampleFormat = 0, _state = 0;
    bool encode(uint8_t* data) const;
    bool decode(const uint8_t* data);
    static uint32_t checksum(const uint8_t* data, uint32_t length);
};
