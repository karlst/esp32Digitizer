// Copyright (C) 2026 Karl Stock
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file RecordingDisk.h
 * Separate board-specific card setup from the reusable file-writing code.
 */
#pragma once

/**
 * The backend calls prepare to make the card's filesystem available. The board
 * implementation supplies pins, power and filesystem setup. format=true permits
 * erasing and formatting the card; false must preserve it. getDrive supplies
 * the filesystem drive prefix, such as "0:", used to form recording paths.
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
