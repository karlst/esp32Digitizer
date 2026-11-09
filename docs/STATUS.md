# Project status

Updated November 7, 2026. This is the single current status document. Dated
measurements and the reasons for earlier decisions are in
[test history](P4-EARLY-TEST-HISTORY.md).

## Current timing work - November 7

Start with the [sample timing design](SAMPLE-TIMING-DESIGN.md) for FSYNC/PPS
alignment, tBlock fields, sample-time readback, and established accuracy limits.

**The new tBlock/sBlock format passed its 60-second hardware test** in
`captures/fsync-association/20261107-153348`. All 60 PPS boundaries succeeded;
59 complete intervals retained 60,415,138 samples. Full SD readback verified
181,248,550 serialized bytes, CRC32 `0a79e9d4`, with zero errors/unsaved bytes.
Staging peaked at 1,026,927 sample slots; the 16-MiB write ring peaked at
1,818,316 bytes. Maximum write/flush duration was 110,850 us.
See [today's complete record](2026-11-07-TIMING-AND-RECORDING.md) for the
measurements, failed attempts, performance fixes, ownership and remaining limits.

The 60-second combined PPS/sample/SD test passed. Run
`captures/fsync-association/20261107-135357` captured/mapped all 60 simulated PPS
pulses, checked sample numbering through two timer wraps, and saved/read back
184,338,333 bytes with CRC32 `4a00c092`. No capture/storage losses were reported.
Ring peak was 1,969,541 of 16,777,216 bytes. See the
[diagnostic guide](FSYNC-ASSOCIATION-TEST.md) for details and measurement limits.

A decoder regression initially caused receive-buffer drops: startup additions
stopped the compiler inlining frame-start processing. The normal path is inline
again, with startup/error handling separate. Karl passed decoder checks and then
the combined hardware test. Diagnostic timing counters remain available.

Current wiring: translated FSYNC to GPIO1; Rigol simulated PPS to GPIO6,
1 Hz, 10% duty, verified 0–3.3 V. The same signal feeds CH0P through the verified
divider; CH0N is grounded. P4 uses the FSYNC diagnostic firmware, COM3; Feather
uses external-source bench firmware, COM4. Karl runs hardware and Git commands.

Adjacent boundary capture also passed in `20261107-145319`: 60 boundaries,
zero failures/missing pulses, two timer wraps, 184,341,357 sample bytes read back,
CRC32 `14dadbca`, ring peak 540,669 bytes. Moving the short capture routine into
internal instruction RAM preceded this successful run; flash-based attempts
had skipped edges and timed out. Timing limits were not loosened.

The version-1 timed file path is now connected behind a separate
`--timed-sd-test` / console `t` selection. `TimedIntervalBuffer` stages packed
samples until a closing PPS boundary is known; `TimedRecorder` formats complete
intervals and writes them through the reusable buffered writer/FatFs adapter.
Desktop checks were expanded for staging, exact sample continuity and rotation.
Karl ran the desktop checks and firmware builds, followed by the passing
new-format run above. Essential changes include separate formatting/disk tasks,
regular task sleeps, aligned internal DMA scratch, and internal instruction RAM
placement for the decoder hot path. The old `--pps-sd-test` remains available.

The new 60-second bench run uses a fresh single-slot file pool, 60 pairs/file,
and a synthetic 2000-01-01 UTC origin. It normally retains 59 complete intervals.
The agreed production defaults remain 10 circular files, but this first live
verification deliberately does not overwrite slots. Real GPS is not yet needed;
GPS UTC-to-PPS association remains unverified. See [file format](TIMED-FILE-FORMAT.md).
The next useful check is PC readback of the actual `.DG` file with the existing
Python reader. New-format live circular reuse and real GPS remain unverified.

## Earlier timing work - November 5

Read [the timing handoff](TIMING-HANDOFF-2026-11-05.md) before resuming. Five
FSYNC association trials, five ADC delay trials and five same-pulse P4 input
comparisons completed. Karl approved **2553 ns** as the configurable measured
input-to-FSYNC correction, including the translator, for this configuration.
Absolute uncertainty has not been fully quantified; proceed to designing time
information for recorded samples rather than repeating the delay discussion.

The curve and delay results are preserved in
[the two-page report](test-results/2026-11-05-adc-pulse-delay.pdf), also copied to
`output/pdf/`. Raw capture folders and complete statistics are in the handoff.
Latest PC smoke checks: **19/19 passed**.

At that date the P4 used the input-skew calibration image and GPIO1 was connected
to the generator. That wiring was subsequently restored; use the November 7
section above for the current bench configuration.

## Earlier status - October 22

The sections below retain the historical October 22 snapshot. References there
to the latest firmware, pending rebuilds or current bench state are historical;
use the November 7 section above for the present timing work.

## Storage experiment

The earlier [P4 SD file-ring choke test](SD-CHOKE-TEST.md), including UTC/text
headers, passed a five-minute hardware run. It wrote 921,600,000 payload bytes
with no loss or readback mismatches, 19 file changes, 15 reuses, and 2.9% peak
occupancy of a 16-MiB ring. Longest payload write was 60.9 ms, rollover 13.4 ms,
and buffered write/flush 103.1 ms. The earlier 4-MiB run lost 9,338,880 bytes
during a sustained slowdown and recovered; that slowdown did not recur in the
last run, so the improvement cannot be attributed solely to buffer size.

The new source extracts a reusable session/file backend and supplies two P4-only
test choices: fresh disk and add-files. Both use five 15-second files, a 16-MiB
ring and five minutes of generated input. Add-files preserves the original files,
rotates only its new five, and checks original lengths/checksums afterward.
The backend also implements use-existing and stop-when-full, with PC checks.
Karl passed PC checks and built/uploaded the session backend, but its first
fresh-disk hardware run failed: 143,308,800 of 921,600,000 input bytes lost,
100% peak ring occupancy, and 3,461 file changes. All 778,291,200 accepted bytes
were written and retained files matched. Loss-triggered file changes caused
repeated close/open overhead; fragmented writes were another likely contributor.

The correction removes loss-triggered rollover and restores payload-only buffering
and large aligned writes. Final loss totals are saved after recording stops.
New regression checks cover repeated losses and actual disk write sizes. Karl subsequently passed the corrected fresh test at 2% peak buffer
occupancy and add-files below 3%, including readback and old-file preservation.
The third test now passed on hardware: 230,400,000 bytes saved with zero loss,
four changeovers, no reuse, all ten original files unchanged, and both used/full
pool refusal checks passed. Ring peak was 20.4%; minimum unused disk-task stack
was 12,096 bytes after moving file caches off the stack and using a 16-KiB stack.

The latest source integrates real ADC recording with this backend through
`SdRecorder`. The smoke profile adds two 16-second files, uses a 16-MiB ring,
checks both files in stream order, and retains the slow square wave, 10% limit
and HTML plot. P4 smoke version 4 requires UTC from the PC before preparation;
Feather remains version 3. Karl passed PC checks and built/uploaded the integration. Its first hardware run
exhausted the exact 30-second capacity: 30,720,503 samples arrived but only
30,720,000 fit. It crossed one file boundary, with zero reported write errors or
ring overflow and 2.7% peak ring usage. The source now reserves 32 seconds in
two 16-second files while keeping the capture at 30 seconds, and reports capacity
exhaustion explicitly. This correction still needs rebuild/upload and rerun.

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

`adsConfig` defaults to 1,024,000 samples/second; Feather `g` starts that default.
ARM3/READY3 carry the actual rate. `RecordingReporter` supplies events to
`ConsoleRecordingReporter`. Storage preparation is part of start: there is no
separate `p`/`claim` step. Preparation allows 180 seconds; Feather waits 185 seconds.
Neither interval counts toward recording time. Karl reported hardware smoke
success after these earlier refactors; that does not validate today's backend integration.

The next smoke run requires **P4 version 4** and **Feather version 3**. The PC
runner supplies UTC before issuing the start request. Karl's latest P4 upload
was the standalone choke image on COM3; replace it with `build/smoke`,
`sdkconfig.smoke`. The previously used Feather image was `ads127-hardware-smoke`
on COM4. Ports can change. The ADC test also needs its 6 V supply, digital wiring,
DAC-to-ADC coax and A1-to-A2 jumper; do not infer their present state from old runs.

The smoke source stops after 30 seconds (P4 timeout 35 seconds). The new backend
reserves two 16-second files; longer startup profiles reserve eleven five-minute
files. Files retain their reserved sizes and headers state valid payload lengths.
Neither board starts capture at boot; existing card files are preserved.

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


## Selectable hardware-smoke duration (2026-10-22)

The previous 30-second ADC/file-ring hardware run passed (capture
`20261022-112740`). The next revision adds a 30/300/600-second selection menu,
acknowledged by both boards before acquisition. Long selections use five
16-second files with circular reuse, retain bounded waveform summaries, and
verify source checksums for surviving file generations. Earlier overwritten
payloads are not readback-verified. New PC regression checks are included.
This revision has not yet been built or tested; Karl runs those steps and uploads
both Feather smoke version 4 and P4 smoke version 5 before hardware testing.
