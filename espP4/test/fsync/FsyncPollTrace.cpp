// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
// Read mark for accounting, checkpoint for retention, report for output.
#include "FsyncPollTrace.h"
#include "esp_timer.h"
#include <cstdio>

FsyncPollTrace::Stage FsyncPollTrace::_stage = FsyncPollTrace::Stage::application;
int64_t FsyncPollTrace::_changedAt = 0;
uint64_t FsyncPollTrace::_totals[_stageCount] = {};
uint64_t FsyncPollTrace::_previous[_stageCount] = {};
uint64_t FsyncPollTrace::_longest[_stageCount] = {};
uint64_t FsyncPollTrace::_maxGapUs = 0;
bool FsyncPollTrace::_havePrevious = false;

/* **
 * mark - Main task changes activity, charging elapsed microseconds to the
 * previous activity, including preemption. esp_timer_get_time is a monotonic
 * clock read for diagnostics; it is not a hardware sample timestamp.
 */
void FsyncPollTrace::mark(Stage stage)
{
    const int64_t now = esp_timer_get_time();
    if (_changedAt != 0)
    {
        _totals[unsigned(_stage)] += uint64_t(now - _changedAt);
    }
    _changedAt = now;
    _stage = stage;
}

/* **
 * reset - Before arming, discard the previous run's observation history. Keep
 * activity totals intact; the first checkpoint starts the new baseline so
 * preparation and idle time are excluded from the reported interval.
 */
void FsyncPollTrace::reset()
{
    _havePrevious = false;
    _maxGapUs = 0;
    for (unsigned iStage = 0; iStage < _stageCount; ++iStage)
    {
        _longest[iStage] = 0;
    }
}

/* **
 * checkpoint - Main task calls after each register observation. Differences
 * from previous cumulative totals partition that interval into activities.
 * Retain only the longest interval; never print while acquisition is running.
 */
void FsyncPollTrace::checkpoint()
{
    mark(_stage);
    uint64_t gapUs = 0;
    for (unsigned iStage = 0; iStage < _stageCount; ++iStage)
    {
        gapUs += _totals[iStage] - _previous[iStage];
    }
    if (_havePrevious && gapUs > _maxGapUs)
    {
        _maxGapUs = gapUs;
        for (unsigned iStage = 0; iStage < _stageCount; ++iStage)
        {
            _longest[iStage] = _totals[iStage] - _previous[iStage];
        }
    }
    for (unsigned iStage = 0; iStage < _stageCount; ++iStage)
    {
        _previous[iStage] = _totals[iStage];
    }
    _havePrevious = true;
}

/* **
 * report - After acquisition, print microseconds for the longest interval.
 * sleep includes the requested one-tick delay and further scheduler delay.
 * Every column includes preemption; none measures exclusive CPU execution.
 */
void FsyncPollTrace::report()
{
    printf("INDEX POLL longestGapUs=%llu controllerUs=%llu boundariesUs=%llu applicationUs=%llu registersUs=%llu sleepUs=%llu\n",
        (unsigned long long)_maxGapUs,
        (unsigned long long)_longest[unsigned(Stage::controller)],
        (unsigned long long)_longest[unsigned(Stage::boundaries)],
        (unsigned long long)_longest[unsigned(Stage::application)],
        (unsigned long long)_longest[unsigned(Stage::registers)],
        (unsigned long long)_longest[unsigned(Stage::sleep)]);
    printf("INDEX POLL LIMIT: elapsed time includes preemption; sleep includes delayed resumption; does not identify the preempting task or interrupt\n");
}
