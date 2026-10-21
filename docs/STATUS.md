# Project status

Updated October 21, 2026. This is the single current status document. Dated
measurements and the reasons for earlier decisions are in
[test history](P4-EARLY-TEST-HISTORY.md).

## What works

The P4 captures ADS127L18 output at 1,024,000 samples/second and records accepted
samples to SD as unaveraged signed 24-bit values, three bytes per sample.
Feather configures and starts/stops the ADC. For signal tests it also supplies
the DAC voltage and measures that wire through the A1-to-A2 jumper.

- **30-minute recording demonstrated:** CardP run 1844422070 saved
  1,843,184,119 samples / 5,529,552,357 payload bytes, with matching CRC32
  `34e6f7ed` and no reported capture/storage errors. This used the ADC's internal
  short input, not an external known waveform. The old binary was later deleted.
- **End-to-end hardware smoke test passes:** October 20 run folder
  `20261020-100816` contains 30,719,711 samples / 92,159,133 payload bytes,
  29.9997 seconds, matching SD readback, no reported receive errors or ring
  overflow, and all 29 expected level changes. Largest plateau-average voltage
  difference: **6.8% of input swing**, below the approved **10%** limit.
- The smoke report includes a single HTML step plot of A2 input and SD output
  on the same voltage scale. No gain/offset is fitted to make the traces agree.
  Each step represents a measured settled average; its boundary is commanded timing.

These are results of specific runs, not production qualification or proof that
all ADC-generated samples arrived. Karl performed the builds, uploads, and
hardware runs; saved logs and console records support the conclusions.

## Current bench setup

The latest source replaces configuration indexes with `Ads127Config` objects.
`adsConfig` defaults to 1,024,000 samples/second; Feather `g` starts that default.
ARM3/READY3 transmit the actual rate. The recorder no longer accepts a stage ID;
its old header slot is reserved and zeroed. `RecordingReporter` is an interface
with terminal output supplied by `ConsoleRecordingReporter`. Karl's earlier
smoke pass predates these changes. They have not yet been built or tested on
hardware; hardware testing remains deferred.


The current source now prepares storage as part of a start request; the separate
`p` command and `claim()` step have been removed. SD task startup is `initTask()`.
Preparation has a 180-second limit; Feather waits 185 seconds for readiness.
Neither interval counts toward recording time. Getter names and destination-lock
names now follow the requested convention. These changes still need Karl's PC
checks, both firmware builds/uploads, and a fresh hardware run. The hardware-smoke
runner requires version 3 firmware on both boards; earlier results below do not
verify this update.

The last reported uploads are **Feather `ads127-hardware-smoke` on COM4** and
**P4 `build/smoke`, `sdkconfig.smoke` on COM3**. Ports can change after reconnection.
The last run used both USB supplies, ADC 6 V power, the existing digital wiring,
DAC-to-ADC coax, A1-to-A2 jumper, and an exFAT SD card. Do not infer present power
or connection state from this historical report.

The smoke firmware stops after 30 seconds (P4 timeout 35 seconds) and reserves
128 MiB before recording. Longer recording builds retain a 10-GB reservation.
The final file is shortened to its actual contents. Neither board starts capture
at boot; existing card files are not overwritten or deleted.

The source default for Feather remains `ads127-p4-constant`; it is not the same
as the last uploaded smoke environment. Always select the intended experiment.

## Where to start

- [Build tools and commands](BUILDING.md): ESP-IDF 5.5.6 for P4; PlatformIO/Arduino
  for Feather. Includes PowerShell environment activation.
- [Hardware smoke test](HARDWARE-SMOKE.md): wiring, dedicated builds/uploads,
  one run command, report meanings, and acceptance limits.
- [Experiment selection](../espP4/CURRENT-SETUP.md): matching Feather/P4 firmware
  choices for SD, RAM comparison, constant input, and endurance work.
- [PC smoke checks](../test/README.md): thirteen suites; these do not operate boards.
- [Test history](P4-EARLY-TEST-HISTORY.md) and [retained evidence](evidence/README.md):
  dated results and supporting material.
- [Open work](../TODO.md): proposed work and unresolved decisions.
- [Working agreements](../AGENTS.md): coding conventions and Karl's control of
  Git, build/upload commands, and hardware operations.

Application code is under `espP4` and `espFeather`. Definitions used by both
boards live under `common`; the reusable writer is in `espP4/lib/bufferedWriter`.
Hardware tests and profiling live with tests.
The Feather web interface is preserved separately in `espFeather/webApp` and is
excluded from the current terminal build.

## Known limitations and open questions

- Feather A2 measurements fluctuate much more than the ADS127/P4 averages.
  Their converters and averaging differ. Factory-reference calibration on the
  Feather and an assumed 2.5-V ADS127 reference are not precision calibration
  of the complete chain. The smoke test uses each plateau's settled average.
- The lower part of the DAC sine wave flattened when connected to the TI input.
  The electrical cause remains unresolved; source loading is a hypothesis.
  Karl chose to focus on a basic smoke test rather than fix or qualify that behavior.
- Card F overflowed where CardP succeeded. Identical branding does not qualify
  every card; the underlying difference remains unproven.
- Missing complete frames and the final incomplete DMA block can escape the
  counters. A matching SD checksum establishes consistency of accepted bytes
  with readback, not exact generated-sample accounting or power-cycle persistence.
- Intermittent post-upload READY/DONE handshake trouble has been cleared by
  resetting P4, but its cause has not been diagnosed.
- Production error policy, card qualification, and migration of Feather duties
  remain open. ADC data CRC and a physical known-digital-sequence test are proposals.

## Evidence and next work

Older retained text logs and selected reports are under `docs/evidence`.
New captures, including the October 20 smoke runs, are local ignored files under
`captures`; they are not automatically included in a commit. This status records
the latest numerical result without claiming that a fresh clone contains its logs.

Karl accepted the smoke-test output and plot as sufficient for the current task
and intends to commit/push. Git operations remain his responsibility. Continue
source review and discuss further changes before implementing them; no additional
hardware experiment is scheduled by this document.

## Source refactor verification

October 20 source changes introduce `RecordingApplication` on P4 and
`AcquisitionApplication` on Feather, with separate ordinary/test startup files.
Classes and their filenames use PascalCase; members use leading underscores;
instances use the class name in camelCase. Feather ADC checks and fake hardware
live in Feather tests.
[Data flow](DATA-FLOW.md) documents the actual transfer and verification calls.

Karl subsequently reported all twelve PC checks passed, P4 and both Feather
builds succeeded, and a fresh 30-second hardware smoke test passed after uploads.
These verify that earlier refactor at smoke-test level. Karl also reported PC
checks passing after the receiver-interface naming cleanup, without another
hardware run.

October 21 changes separate P4 control, reporting and data transfer into
`RecordingController`, `RecordingApplication`, `RecordingReporter`, `AdcReceiver`
and `SampleDecoder`. The observer no longer handles commands. SD formatting
moves into the reporter; file layout, buffer sizes and task placement are retained.
`shared` is replaced by project-owned locations and a small `common` folder.
See [Recording classes](RECORDING-CLASSES.md).

A new PC suite exercises lifecycle sequencing and injected failures. **The
October 21 refactor has not yet been built or tested.** Karl must run the PC
smoke checks, P4 and Feather builds, and hardware smoke after uploading.
