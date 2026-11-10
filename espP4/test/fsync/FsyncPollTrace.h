// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// Bench-only elapsed-time accounting for the task that observes FSYNC.
#pragma once
#include <cstdint>

/**
 * Core 0 main task marks its activities. Wall time includes interruptions and
 * time when other tasks run. Only that task accesses this state: no locks are
 * needed. Retain the longest observation interval in fixed RAM, then print its
 * breakdown after shutdown. This locates a delay in the control sequence; it
 * does not identify which task or interrupt took CPU time during that delay.
 */
class FsyncPollTrace
{
public:
    enum class Stage { controller, boundaries, application, registers, sleep, count };
    static void mark(Stage stage);
    static void reset();
    static void checkpoint();
    static void report();
private:
    static constexpr unsigned _stageCount = unsigned(Stage::count);
    static Stage _stage;
    static int64_t _changedAt;
    static uint64_t _totals[_stageCount], _previous[_stageCount], _longest[_stageCount];
    static uint64_t _maxGapUs;
    static bool _havePrevious;
};
