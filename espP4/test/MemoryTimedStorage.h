// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// Disk-operation simulation below the real byte ring, distinct from queue input.
#pragma once
#include "TimedSlotStorage.h"
#include <vector>
#include <algorithm>

/**
 * Simulate completed file operations and inject failures. Tests inspect calls
 * before pumping to prove that submission has not touched storage. Content
 * stays in PC memory; this does not simulate card latency or DMA hardware.
 */
class MemoryTimedStorage : public TimedSlotStorage
{
public:
    std::vector<std::vector<uint8_t>> _files;
    std::vector<size_t> _writes;
    uint64_t _utcSample = 0;
    uint32_t _operations = 0, _opens = 0, _closes = 0;
    bool _failOpen = false, _shortWrite = false, _failClose = false;

    /* **
     * openSlot - Replace one simulated prepared slot and record disk activity.
     */
    bool openSlot(uint32_t iSlot, uint64_t utcSample) override
    {
        ++_operations;
        const bool retVal = !_opened && !_failOpen;
        if (retVal)
        {
            if (_files.size() <= iSlot)
            {
                _files.resize(iSlot + 1);
            }
            _files[iSlot].clear();
            _iSlot = iSlot;
            _utcSample = utcSample;
            _opened = true;
            ++_opens;
        }
        return retVal;
    }

    /* **
     * write - Store an accepted prefix, optionally refusing the final byte.
     */
    size_t write(const uint8_t* data, size_t bytes) override
    {
        ++_operations;
        _writes.push_back(bytes);
        size_t retVal = _opened ? bytes : 0;
        if (retVal && _shortWrite)
        {
            --retVal;
        }
        if (retVal)
        {
            _files[_iSlot].insert(_files[_iSlot].end(), data, data + retVal);
        }
        return retVal;
    }

    /* **
     * flush - Count synchronization; close failure models an unsuccessful sync.
     */
    bool flush() override
    {
        ++_operations;
        return !_failClose;
    }

    /* **
     * closeSlot - Record close even if it reports a simulated storage failure.
     */
    bool closeSlot() override
    {
        ++_operations;
        ++_closes;
        const bool retVal = _opened && !_failClose;
        _opened = false;
        return retVal;
    }
private:
    uint32_t _iSlot = 0;
    bool _opened = false;
};
