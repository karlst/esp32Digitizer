// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Bench adapter connecting the existing timing diagnostic to the timed writer.
 * Reading path: command selects 't'; prepare requests storage; poll waits for
 * readiness and delivers timing boundaries; submit copies decoder batches;
 * finish requests drain and verification after acquisition has stopped.
 * Control methods run on Core 0; submit runs in the Core 1 decoder task.
 * Other test selections delegate to the existing FsyncSdTest, preserving the
 * sample-only comparison path. Neither path is a production GPS time source.
 *
 * The timing service observes FSYNC about every millisecond but supplies only
 * one boundary per PPS. poll copies each completed boundary to TimedRecorder,
 * which combines consecutive boundaries with the intervening decoded samples.
 * Nothing here substitutes the time poll runs for a captured PPS/FSYNC tick.
 */
#include "FsyncTimedTest.h"
#include "RecordingApplication.h"
#include "sdkconfig.h"
#include "freertos/task.h"
#include <cstdio>

/* **
 * FsyncTimedTest - Borrow static services; no SD allocation during construction.
 */
FsyncTimedTest::FsyncTimedTest(FsyncAssociationTest& timing, FsyncSdTest& previous, TimedRecorder& recorder)
    : _timing(timing), _previous(previous), _recorder(recorder)
{
}

/* **
 * command - Control task accepts t while idle before another destination has
 * been chosen. Files use synthetic 2000-01-01 UTC for the first retained PPS.
 * This deliberately tests serialization without claiming GPS synchronization.
 * Selecting this mode raises the calling Core 0 control task to priority 5,
 * above the disk and formatting tasks. Reset restores the default priority.
 */
void FsyncTimedTest::command(RecordingApplication& application, char key)
{
    if (key == 't')
    {
#if CONFIG_P4_PULSE_DELAY
        printf("TIMED rejected: use FSYNC firmware, not pulse-delay firmware\n");
#else
        if (!_otherSelected && !_selected && application.getIdle() &&
            !application.isRecordingDestinationLocked() && application.setTimeout(305000) && _recorder.initialize())
        {
            _selected = true;

            // nullptr means the calling task: the main control loop on Core 0.
            // Higher numbers run first when tasks are ready. Priority 5 lets
            // timing observations preempt the priority-4 writer and priority-3
            // formatter. The main loop still sleeps one tick on every pass,
            // allowing both storage tasks to run. Core 1 decoding stays at 10.
            // This is a bench scheduling experiment, not a timing guarantee;
            // interrupt handling and critical sections can still delay us.
            const UBaseType_t previousPriority = uxTaskPriorityGet(nullptr);
            vTaskPrioritySet(nullptr, 5);
            printf("TIMED SCHED controlBefore=%lu controlNow=%lu disk=4 format=3 decoder=10\n",
                (unsigned long)previousPriority, (unsigned long)uxTaskPriorityGet(nullptr));

            // Extend the application's separate reception watchdog as well as
            // the diagnostic target. Disk preparation happens before this timer.
            _timing.command(application, 'l');
            printf("TIMED MODE version=2 seconds=300 pairsPerFile=60 files=5 syntheticUtc=1\n");
        }
        else
        {
            printf("TIMED rejected: reset P4 before selecting timed-format mode\n");
        }
#endif
    }
    else if (_selected && (key == 'd' || key == 'p' || key == 'l' || key == 'n' || key == 'r'))
    {
        printf("TIMED rejected: reset P4 before changing mode\n");
    }
    else
    {
        _otherSelected = _otherSelected || key == 'd';
        _previous.command(application, key);
    }
}

/* **
 * prepare - Request a fresh preserved pool with the agreed 60 pairs per file.
 * This five-minute verification uses five slots and stops rather than overwrites;
 * application defaults remain 10 circular slots in TimedRecordingConfig.
 */
bool FsyncTimedTest::prepare(uint32_t run, const Ads127Config& config)
{
    bool retVal;
    if (_selected)
    {
        _run = run;
        _config = config;
        _nextBoundary = 0;
        TimedRecordingConfig settings;

        // Use five fresh slots, rotating after 60 complete pairs per file.
        // About 300 PPS edges give 299 closed pairs: four full files and a
        // shorter fifth file. Preserve every file for the aggregate CRC readback.
        // This fixed UTC value identifies a synthetic origin, not today's date.
        settings._pairsPerFile = 60;
        settings._fileCount = 5;
        settings._overwrite = false;
        retVal = config.matches(ads127DefaultConfig) && _recorder.prepare(run, 946684800, settings);
        _preparing = retVal;
        _failed = !retVal;
    }
    else
    {
        retVal = _previous.prepare(run, config);
    }
    return retVal;
}

/* **
 * poll - Control task arms capture after SD preparation, then queues each new
 * actual boundary once. Timing polling occurs through RecordingApplication's
 * observer; a one-loop delay here affects delivery, never captured tick values.
 */
void FsyncTimedTest::poll()
{
    if (!_selected)
    {
        _previous.poll();
    }
    else
    {
        if (_preparing && _recorder.getReady())
        {
            _prepared = _timing.prepare(_run, _config);
            _failed = !_prepared;
            _preparing = false;
        }

        // Copy each completed boundary once. getBoundary does not return a live
        // hardware register: the timing diagnostic already saved the captures.
        // Refusal stops further delivery rather than dropping an interval.
        TimedBoundary boundary;
        while (_prepared && !_failed && _timing.getBoundary(_nextBoundary, boundary))
        {
            _failed = !_recorder.addBoundary(boundary);
            ++_nextBoundary;
        }
    }
}

/* **
 * getReady - Confirm both destination memory/storage and stopped-source timing setup.
 */
bool FsyncTimedTest::getReady() const
{
    return _selected ? _prepared && _recorder.getReady() && _timing.getReady() : _previous.getReady();
}

/* **
 * cancelPreparation - Stop pending work; disk cleanup remains in its own task.
 */
void FsyncTimedTest::cancelPreparation()
{
    if (_selected)
    {
        _preparing = false;
        _timing.cancelPreparation();
        _recorder.finish(false);
    }
    else
    {
        _previous.cancelPreparation();
    }
}

/* **
 * submit - Decoder copies the same batch into diagnostic RAM and interval staging.
 * Either refusal is a capture failure; no retry can duplicate a partially accepted batch.
 */
bool FsyncTimedTest::submit(const uint32_t* words, size_t count)
{
    bool retVal = false;
    if (_selected)
    {
        // Both destinations copy before returning; neither retains words.
        // Still call the recorder after a diagnostic refusal so the controller
        // sees one failed acquisition, not a retry of a partially copied batch.
        retVal = _timing.submit(words, count);
        retVal = _recorder.submit(words, count) && retVal;
    }
    else
    {
        retVal = _previous.submit(words, count);
    }
    return retVal;
}

/* **
 * getFailed - Control task combines preparation, timing and asynchronous write faults.
 */
bool FsyncTimedTest::getFailed() const
{
    return _selected ? _failed || _recorder.getFailed() || (_prepared && _timing.getFailed()) : _previous.getFailed();
}

/* **
 * getFull - The five-minute timing target requests the normal source stop.
 * About 300 observed PPS edges supply 299 complete intervals, not 300 intervals.
 */
bool FsyncTimedTest::getFull() const
{
    return _selected ? _timing.getFull() : _previous.getFull();
}

/* **
 * finish - Source and decoder have stopped. Deliver any final boundary, validate
 * the stopped endpoint, then request writer drain. No new boundary is delivered
 * after this point. The host must await TIMED SAVED as well as PPS RESULT.
 */
void FsyncTimedTest::finish(uint32_t run, uint32_t rate, bool clean, uint32_t samples)
{
    if (_selected)
    {
        poll();
        _timing.finish(run, rate, clean, samples);
        _prepared = false;
        _recorder.finish(clean && !getFailed() && !_timing.getFailed());
    }
    else
    {
        _previous.finish(run, rate, clean, samples);
    }
}
