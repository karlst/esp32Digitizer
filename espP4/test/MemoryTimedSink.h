// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// PC-only file-slot simulation; no board or filesystem access.
#pragma once
#include "TimedFileSink.h"
#include <vector>

/**
 * Copy submitted bytes into numbered in-memory files. Opening an existing slot
 * replaces only that slot. Tests can reject one operation to ensure a failed
 * submission cannot acquire a normal-completion marker on a later finish call.
 */
class MemoryTimedSink : public TimedFileSink
{
public:
    std::vector<std::vector<uint8_t>> _files;
    std::vector<uint32_t> _openedSlots;
    std::vector<uint64_t> _sampleSeconds;
    uint32_t _closed = 0, _calls = 0, _rejectCall = 0;

    /* **
     * beginFile - Simulate selecting a prepared slot; record its reuse history.
     */
    bool beginFile(uint32_t iSlot, uint64_t utcSample) override
    {
        const bool retVal = accept() && !_opened;
        if (retVal)
        {
            if (_files.size() <= iSlot)
            {
                _files.resize(iSlot + 1);
            }
            _files[iSlot].clear();
            _iSlot = iSlot;
            _openedSlots.push_back(iSlot);
            _sampleSeconds.push_back(utcSample);
            _opened = true;
        }
        return retVal;
    }

    /* **
     * submit - Copy exactly the accepted bytes, with an optional injected fault.
     */
    bool submit(const uint8_t* data, size_t bytes) override
    {
        const bool retVal = accept() && _opened;
        if (retVal)
        {
            _files[_iSlot].insert(_files[_iSlot].end(), data, data + bytes);
        }
        return retVal;
    }

    /* **
     * endFile - Count closure only if the request is accepted for an open slot.
     */
    bool endFile() override
    {
        const bool retVal = accept() && _opened;
        if (retVal)
        {
            ++_closed;
            _opened = false;
        }
        return retVal;
    }

private:
    /* **
     * accept - Reject one specified one-based call, or none when rejectCall=0.
     */
    bool accept()
    {
        ++_calls;
        return _calls != _rejectCall;
    }
    uint32_t _iSlot = 0;
    bool _opened = false;
};
