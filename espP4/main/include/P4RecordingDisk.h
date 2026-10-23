// Copyright (C) 2026 Karl Stock
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file P4RecordingDisk.h
 * Waveshare P4 card power/pins and filesystem mounting, separate from recording policy.
 */
#pragma once
#include "RecordingDisk.h"
#include "ff.h"
#include "sdmmc_cmd.h"

/**
 * Board-specific SD adapter; initialize once and reuse its mount between sessions.
 */
class P4RecordingDisk : public RecordingDisk
{
public:
    /* **
     * P4RecordingDisk - Construct without touching card power or contents.
     */
    P4RecordingDisk() = default;
    bool prepare(bool format) override;
    const char* getDrive() const override;

private:
    sdmmc_card_t _card = {};
    FATFS _fatfs = {};
    bool _initialized = false, _mounted = false, _attempted = false;
    char _drive[3] = {};
};
