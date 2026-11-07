// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// Submission interface between the timed format and asynchronous file storage.
#pragma once
#include <cstddef>
#include <cstdint>

/**
 * Accept ordered file-boundary requests and copied bytes without disk I/O or
 * waiting on the submitting task. An implementation queues requests for its
 * disk task and retains bytes until writing completes. Acceptance is NOT a
 * completed write. A false result is fatal: never retry a possibly accepted
 * prefix. Slot numbers refer only to files preallocated for this session.
 */
class TimedFileSink
{
public:
    /* **
     * ~TimedFileSink - Permit destruction through the submission interface.
     */
    virtual ~TimedFileSink() = default;
    virtual bool beginFile(uint32_t iSlot) = 0;
    virtual bool submit(const uint8_t* data, size_t bytes) = 0;
    virtual bool endFile() = 0;
};
