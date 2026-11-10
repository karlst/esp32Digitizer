/* Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
 * Dedicated diagnostic startup. Reuse real reception, decoding and source control.
 * Read the object connections below, then FsyncAssociationTest.cpp for the run
 * sequence. The pulse-delay build adds PulseDelayCapture.cpp; the association
 * build can also select PPS capture on GPIO6 at runtime. SD access requires
 * explicit combined-test selection through FsyncSdTest.
 */
#include "FsyncAssociationTest.h"
#include "FsyncSdTest.h"
#include "FsyncTimedTest.h"
#include "FsyncPollTrace.h"
#include "AdcReceiver.h"
#include "SampleDecoder.h"
#include "RecordingApplication.h"
#include "RecordingController.h"
#include "FsyncConsoleReporter.h"
#include "sdkconfig.h"

#if CONFIG_P4_PULSE_DELAY
static PulseDelayCapture pulseDelayCapture;
#endif

static FsyncConsoleReporter fsyncConsoleReporter;

/* **
 * getTimingSdConfig - Reserve one new 65-second file for a 60-second capture,
 * leaving shutdown headroom. addFiles preserves the card format and existing
 * recordings; stopWhenFull never recycles a previous recording. This is the
 * existing three-byte sample format, not the proposed tBlock/sBlock format.
 */
static RecordingSessionConfig getTimingSdConfig()
{
    RecordingSessionConfig recordingSessionConfig;
    recordingSessionConfig._preparation = RecordingSessionConfig::Preparation::addFiles;
    recordingSessionConfig._fullPolicy = RecordingSessionConfig::FullPolicy::stopWhenFull;
    recordingSessionConfig._secondsPerFile = 65;
    recordingSessionConfig._fileCount = 1;
    return recordingSessionConfig;
}

static SdRecorder sdRecorder(getTimingSdConfig(), nullptr, &fsyncConsoleReporter);
static SdCaptureDestination sdCaptureDestination(sdRecorder);
static FsyncAssociationTest fsyncAssociationTest;
static FsyncSdTest fsyncSdTest(fsyncAssociationTest, sdRecorder, sdCaptureDestination);
static TimedRecorder timedRecorder;
static FsyncTimedTest fsyncTimedTest(fsyncAssociationTest, fsyncSdTest, timedRecorder);
// Receiver collects clocked pin states; decoder turns them into ADC words.
// The wrapper defaults to RAM and enables the ordinary recorder only on command.
static AdcReceiver adcReceiver;
static SampleDecoder sampleDecoder(adcReceiver, &fsyncAssociationTest);
// Ordinary PPS mode needs sixty seconds plus source-stop allowance. Timed SD
// selection raises this reception timeout to 305 seconds while idle.
// Ordinary association/delay runs still stop at their original word target.
static RecordingApplication recordingApplication(adcReceiver, sampleDecoder,
    fsyncTimedTest, 65000, &fsyncAssociationTest);
static RecordingController recordingController(recordingApplication,
    fsyncConsoleReporter, &fsyncTimedTest);

/* **
 * app_main - Allocate the ordinary receiver/decoder and diagnostic services.
 * Wait for Feather commands; never start the ADC at boot. SD task/ring
 * allocation occurs only on explicit combined-test selection; file preparation
 * starts later on the real ARM command. The test is the sole MCPWM group-0 capture owner.
 * The separate pulse-delay configuration also supplies the GPIO6 capture
 * service. It shares the timer; no pins are configured as pulse outputs.
 */
extern "C" void app_main()
{
    recordingApplication.initialize();
#if CONFIG_P4_PULSE_DELAY
    fsyncAssociationTest.initialize(sampleDecoder.getFrameDecoder(), &pulseDelayCapture);
#else
    fsyncAssociationTest.initialize(sampleDecoder.getFrameDecoder());
#endif
    recordingController.initialize();
    for (;;)
    {
        // The controller reads console/Feather commands. Application polling
        // advances acquisition and calls diagnostic poll/stop/finish hooks.
        // Yield one scheduler tick so other tasks can run; this is not a
        // sample-timing delay and does not determine hardware capture times.
        // Fixed-memory diagnostics include time spent waiting for other tasks.
        FsyncPollTrace::mark(FsyncPollTrace::Stage::controller);
        recordingController.poll();
        FsyncPollTrace::mark(FsyncPollTrace::Stage::boundaries);
        fsyncTimedTest.poll();
        FsyncPollTrace::mark(FsyncPollTrace::Stage::application);
        recordingApplication.poll();
        FsyncPollTrace::mark(FsyncPollTrace::Stage::sleep);
        vTaskDelay(1);
    }
}
