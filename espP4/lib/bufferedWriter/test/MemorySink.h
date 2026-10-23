/*
 * Copyright (C) 2026 Karl Stock
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of ESP32 Digitizer.
 * You may redistribute and/or modify it under the GNU General Public License
 * as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 *
 * Distributed WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License in the repository LICENSE file or <https://www.gnu.org/licenses/>.
 */

/**
 * @file MemorySink.h
 * In-memory byte destination for the reusable writer's desktop tests.
 */
#pragma once
#include <BufferedWriter.h>
#include <vector>
#include <cassert>
extern uint64_t writerTestUs;

/**
 * In-memory storage with controllable latency and failures.
 */
class MemorySink : public ByteSink
{
public:
    std::vector<uint8_t> _bytes;
    bool _shortWrite = false, _flushOk = true;
    BufferedWriter* _bufferedWriterDuringWrite = nullptr;

    /* **
     * write - Copy accepted bytes; optionally force producer activity while blocked.
     * This fake replaces an actual filesystem. bytes records what storage accepted;
     * shortWrite forces a one-byte-short result. duringWrite asks the producer to
     * fill the ring WHILE this write still owns its input memory. That deliberately
     * stresses the rule that tail must not advance before a write returns.
     */
    size_t write(const uint8_t* data, size_t length) override
    {
        // Retain a reference copy to detect whether concurrent submissions illegally
        // reused the block still being written.
        const std::vector<uint8_t> before(data, data + length);
        if (_bufferedWriterDuringWrite)
        {
            const uint8_t sample[4] = {9, 8, 7, 6};
            while (_bufferedWriterDuringWrite->submit(sample, 4))
            {
            }

            // Buffer space must not be released until this call returns.
            assert(std::vector<uint8_t>(data, data + length) == before);
            _bufferedWriterDuringWrite = nullptr;
        }

        // Pretend the write took 12 ms and accept either the full span or a short
        // prefix. The real writer should report exactly those simulated results.
        writerTestUs += 12000;
        const size_t retVal = _shortWrite ? length - 1 : length;
        _bytes.insert(_bytes.end(), data, data + retVal);
        return retVal;
    }

    /* **
     * flush - Make the final flush slower than writes to test retained maxima.
     * Advance simulated time, not real wall-clock time; no 45-ms sleep occurs.
     * Returning flushOk lets the test prove a failed synchronization is not hidden.
     */
    bool flush() override
    {
        writerTestUs += 45000;
        return _flushOk;
    }
};

