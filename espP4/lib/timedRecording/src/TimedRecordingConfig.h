// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// Runtime choices, fixed for the duration of a recording session.
#pragma once
#include <cstdint>

/**
 * Defaults agreed for timed recording. A future serial/web command can provide
 * another value through TimedRecordingStream::start; no UI parsing belongs in
 * this component. Only this session's allocated slots may be overwritten.
 */
struct TimedRecordingConfig
{
    uint32_t _pairsPerFile = 60;
    uint32_t _fileCount = 10;
    bool _overwrite = true;
    int32_t _delayNs = 2553;
};
