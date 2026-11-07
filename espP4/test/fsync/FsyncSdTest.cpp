/* Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
 * Reading path: command d selects the test; prepare asks the existing recorder
 * for a new file; poll waits for disk readiness before arming timing capture;
 * submit copies batches through both ordinary services; finish requests SD
 * drain/readback and prints timing results. PC must wait for both PPS RESULT
 * and SD SAVED. ADC127 DONE alone does not establish storage completion.
 */
#include "FsyncSdTest.h"
#include "RecordingApplication.h"
#include "sdkconfig.h"
#include <cstdio>

/* **
 * FsyncSdTest - Store references with static lifetimes; do not allocate memory,
 * mount a card, or create files during construction.
 */
FsyncSdTest::FsyncSdTest(FsyncAssociationTest& fsyncAssociationTest, SdRecorder& sdRecorder,
    SdCaptureDestination& sdCaptureDestination)
    : _fsyncAssociationTest(fsyncAssociationTest), _sdRecorder(sdRecorder),
      _sdCaptureDestination(sdCaptureDestination)
{
}

/* **
 * command - Core 0 accepts d only before an actual capture locks the destination.
 * The caller selects d after the idle link check; that probe does not lock the
 * destination. Allocate the recorder task
 * and ring once, then select the existing sixty-second PPS diagnostic. No card
 * operation occurs until prepare. Other RAM commands remain unchanged before d;
 * after d, reset is required to prevent an accidental RAM request writing files.
 */
void FsyncSdTest::command(RecordingApplication& application, char key)
{
    if (key == 'd')
    {
#if CONFIG_P4_PULSE_DELAY
        printf("PPS SD rejected: use FSYNC firmware, not pulse-delay firmware\n");
#else
        if (application.getIdle() && !application.isRecordingDestinationLocked())
        {
            if (!_initialized)
            {
                _initialized = _sdRecorder.initTask();
            }
            if (_initialized)
            {
                _enabled = true;
                _fsyncAssociationTest.command(application, 'p');
                printf("PPS SD MODE version=1 seconds=60 preserve=1\n");
            }
            else
            {
                printf("PPS SD rejected: recorder memory/task allocation failed; reset P4\n");
            }
        }
        else
        {
            printf("PPS SD rejected: reset P4 before selecting a new SD run\n");
        }
#endif
    }
    else if (_enabled && (key == 'p' || key == 'n' || key == 'r'))
    {
        printf("PPS SD rejected: reset P4 before changing diagnostic mode\n");
    }
    else
    {
        _fsyncAssociationTest.command(application, key);
    }
}

/* **
 * prepare - Core 0 receives the real Feather ARM request. RAM mode delegates
 * unchanged. SD mode starts asynchronous file preparation and stores parameters
 * for later timing setup; true means accepted, not ready to receive samples.
 */
bool FsyncSdTest::prepare(uint32_t run, const Ads127Config& config)
{
    bool retVal = false;
    if (_enabled)
    {
        _run = run;
        _config = config;
        _hardwarePrepared = false;
        _failed = false;
        retVal = config.matches(ads127DefaultConfig) && _sdCaptureDestination.prepare(run, config);
        _preparing = retVal;
        _failed = !retVal;
    }
    else
    {
        retVal = _fsyncAssociationTest.prepare(run, config);
    }
    return retVal;
}

/* **
 * poll - Startup calls once per Core 0 loop before RecordingApplication::poll.
 * Once disk preparation finishes, arm FSYNC/PPS while the ADC is still stopped.
 * This prevents file preallocation time from consuming the capture deadline or
 * filling the PPS event array. Both services must be ready before receiver arm.
 */
void FsyncSdTest::poll()
{
    if (_enabled && _preparing)
    {
        if (_sdCaptureDestination.getFailed())
        {
            _failed = true;
            _preparing = false;
        }
        else if (_sdCaptureDestination.getReady())
        {
            _hardwarePrepared = _fsyncAssociationTest.prepare(_run, _config);
            _failed = !_hardwarePrepared;
            _preparing = false;
        }
    }
}

/* **
 * getReady - Core 0 permits ADC setup only after storage and timing are ready.
 */
bool FsyncSdTest::getReady() const
{
    const bool retVal = _enabled ? _hardwarePrepared && _sdCaptureDestination.getReady() &&
        _fsyncAssociationTest.getReady() : _fsyncAssociationTest.getReady();
    return retVal;
}

/* **
 * cancelPreparation - Core 0 cancels disk work and any armed timing capture.
 * Disk cleanup stays asynchronous; no writes occur from this calling task.
 */
void FsyncSdTest::cancelPreparation()
{
    if (_enabled)
    {
        _preparing = false;
        _sdCaptureDestination.cancelPreparation();
    }
    _fsyncAssociationTest.cancelPreparation();
    _hardwarePrepared = false;
}

/* **
 * submit - Core 1 supplies decoded words including status. The timing diagnostic
 * retains its original small sample window and counts every word. The ordinary
 * SD adapter packs three bytes per sample and copies them into the existing
 * ring; it neither waits for nor performs disk writes. No caller pointer is
 * retained. A refusal fails the decoder path; never retry or silently drop data.
 */
bool FsyncSdTest::submit(const uint32_t* words, size_t count)
{
    bool retVal = _fsyncAssociationTest.submit(words, count);
    if (_enabled)
    {
        retVal = _sdCaptureDestination.submit(words, count) && retVal;
    }
    return retVal;
}

/* **
 * getFailed - Core 0 combines hardware/preparation and thread-safe recorder faults.
 * Before timing prepare, do not inspect stale timing state from a prior run.
 */
bool FsyncSdTest::getFailed() const
{
    const bool retVal = _enabled ? _failed || _sdCaptureDestination.getFailed() ||
        (_hardwarePrepared && _fsyncAssociationTest.getFailed()) : _fsyncAssociationTest.getFailed();
    return retVal;
}

/* **
 * getFull - The timing test ends after sixty seconds. A prematurely full file
 * also requests shutdown; subsequent sample-count checks reject that short run.
 */
bool FsyncSdTest::getFull() const
{
    const bool retVal = _fsyncAssociationTest.getFull() ||
        (_enabled && _sdCaptureDestination.getFull());
    return retVal;
}

/* **
 * finish - Core 0 calls after the source stopped and Core 1 drained decoding.
 * Request the real recorder's flush, close and CRC readback first, then print
 * timing evidence. Either side can fail independently. The PC must require both
 * PPS success and SD SAVED with zero lost/unsaved bytes before calling it a pass.
 */
void FsyncSdTest::finish(uint32_t run, uint32_t rate, bool clean, uint32_t samples)
{
    if (_enabled)
    {
        _sdCaptureDestination.finish(run, rate, clean, samples);
    }
    _fsyncAssociationTest.finish(run, rate, clean, samples);
}
