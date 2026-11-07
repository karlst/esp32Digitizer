// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// Disk-task interface for a pool of prepared files; no ADC or board pin settings.
#pragma once
#include "ByteSink.h"

/**
 * Operate only on files selected for this recording session. All methods may
 * block on storage and therefore belong exclusively to the disk task. openSlot
 * positions an allocated file at byte zero; closeSlot syncs and closes it. The
 * inherited write/flush methods never retain the supplied buffer after return.
 */
class TimedSlotStorage : public ByteSink
{
public:
    virtual bool openSlot(uint32_t iSlot) = 0;
    virtual bool closeSlot() = 0;
    /* **
     * getFailed - Report errors that may accompany a full accepted byte count.
     * Simple adapters can rely on short writes/false returns instead.
     */
    virtual bool getFailed() const
    {
        return false;
    }
};
