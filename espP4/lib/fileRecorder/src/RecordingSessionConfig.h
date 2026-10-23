// Copyright (C) 2026 Karl Stock
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file RecordingSessionConfig.h
 * Parameters supplied before each recording session, never during capture.
 */
#pragma once
#include <cstdint>

/**
 * Settings for one recording session. Calculate each new file's capacity from
 * seconds per file, samples per second and bytes per sample. Switch files when
 * that byte capacity is reached, not when a timer expires.
 *
 * formatAndAllocate erases the card and creates files. addFiles preserves
 * existing files and creates _fileCount additional files. useExisting selects
 * compatible prepared files and uses their existing sizes, ignoring _fileCount
 * and _secondsPerFile. With stopWhenFull it selects unused files only;
 * overwriteOldest also allows previously recorded files.
 *
 * When the selected files fill, stopWhenFull stops accepting data.
 * overwriteOldest reuses only files selected for this session; it does not
 * overwrite unrelated recordings.
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
