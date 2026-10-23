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
 * @file ByteSink.h
 * Small storage interface used by the reusable buffered writer.
 */
#pragma once
#include <cstddef>
#include <cstdint>

/**
 * Interface used by BufferedWriter to write bytes and finish pending storage
 * operations. A sink is simply a destination for data. FileRecordingBackend
 * supplies the implementation in this project; PC tests supply an in-memory
 * implementation.
 *
 * write returns the number of bytes actually accepted, including a short count
 * on failure. flush returns whether pending storage writes completed
 * successfully. Neither is called by submit. The default destructor does not
 * save or close anything.
 */
class ByteSink
{
public:
    /* **
     * ~ByteSink - Allow destruction through the interface after the writer stops.
     */
    virtual ~ByteSink() = default;

    // The disk-writing task calls this. Read length bytes at data and
    // return how many the storage API accepted. Do not retain data after returning:
    // the ring can reuse that memory immediately. A DMA adapter must wait for DMA
    // completion here before returning (DMA = hardware transfers without CPU copying).
    virtual size_t write(const uint8_t* data, size_t length) = 0;

    // Finish pending storage synchronization. Return false on failure. This does
    // not mean close the file; the project adapter controls file lifetime.
    virtual bool flush() = 0;
};
