// Copyright (C) 2026 Karl Stock
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file RecordingSessionConfig.h
 * Parameters supplied before each recording session, never during capture.
 */
#pragma once
#include <cstdint>

/**
 * Settings supplied before one run of the digitizer, called a session.
 * New file size = seconds per file x samples per second x bytes per sample,
 * plus a 512-byte header. Switch files when their sample-byte capacity is full,
 * not when a timer expires. For example, 15 x 1,024,000 x 3 reserves 46,080,000
 * sample bytes per file. File count and duration are choices made by the caller.
 *
 * formatAndAllocate erases the card and creates files. addFiles preserves
 * existing files and creates _fileCount additional files. useExisting selects
 * compatible prepared files and uses their existing sizes, ignoring _fileCount
 * and _secondsPerFile. With stopWhenFull it selects unused files only;
 * overwriteOldest also allows previously recorded files.
 *
 * When the selected files fill, stopWhenFull stops accepting data.
 * overwriteOldest returns to the first selected file and writes it again.
 * With addFiles, only the newly created files are selected; previous recordings
 * are preserved. With useExisting, compatible older recordings may be selected
 * for reuse. Unrelated files are never part of this pool.
 */
struct RecordingSessionConfig
{
    enum class Preparation
    {
        formatAndAllocate,
        addFiles,
        useExisting
    };
    enum class FullPolicy
    {
        overwriteOldest,
        stopWhenFull
    };
    Preparation _preparation = Preparation::useExisting;
    FullPolicy _fullPolicy = FullPolicy::stopWhenFull;
    uint32_t _secondsPerFile = 15, _fileCount = 5;
    uint32_t _sampleRate = 1024000, _bytesPerSample = 3, _sampleFormat = 1;
};
