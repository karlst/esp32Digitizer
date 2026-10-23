// Copyright (C) 2026 Karl Stock
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file RecordingDisk.h
 * Board adapter contract; file management does not know card pins or power.
 */
#pragma once

/**
 * Mount an existing filesystem, or explicitly format it when requested.
 */
class RecordingDisk
{
public:
    /* **
     * ~RecordingDisk - Permit adapter destruction through the interface; no implicit
     * formatting.
     */
    virtual ~RecordingDisk() = default;
    virtual bool prepare(bool format) = 0;
    virtual const char* getDrive() const = 0;
};
