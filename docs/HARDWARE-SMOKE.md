# Selectable-duration hardware smoke test

This checks Feather DAC -> ADS127 analog input -> P4 -> SD card -> readback.
The boards perform the capture and readback; a PC script coordinates them,
compares the reports, saves detailed logs, and prints a short PASS/FAIL result.
It uses the existing real ADC driver, decoder, recorder, and disk checksum.
Test-specific code is under the board `test` folders and compiled only when selected.

For the Rigol source, see [External generator test](EXTERNAL-GENERATOR-TEST.txt).
That option disables the Feather DAC and compares against scope readings.

## Before building

Run the PC smoke checks from the repository root:

```powershell
.\test\runSmokeChecks.ps1
```

There are fifteen suites, including recording lifecycle checks, readback
parsing, square-wave timing, and rejection of missing, corrupt or mismatched reports.
Passing these checks does not establish hardware performance.

## Build and upload Feather

In ordinary PowerShell, from the repository root:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run --project-dir .\espFeather -e ads127-hardware-smoke
```

After a successful build, close any COM4 monitor and upload:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run --project-dir .\espFeather -e ads127-hardware-smoke -t upload --upload-port COM4
```

The default Feather environment is unchanged; this is an explicitly selected test.

## Build and upload P4

Use the [ESP-IDF PowerShell activation](BUILDING.md), then:

```powershell
Set-Location C:\Users\karls\source\esp32Digitizer\espP4
idf.py -B build/smoke -D SDKCONFIG=sdkconfig.smoke -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.sd.defaults;sdkconfig.smoke.defaults" build
```

This creates a separate smoke configuration and build directory. After a successful
build, close any COM3 monitor and upload:

```powershell
idf.py -B build/smoke -D SDKCONFIG=sdkconfig.smoke -p COM3 flash
```

Neither board starts the signal or recording at boot. Rebuild and upload **both
boards** from the current source before using new report features. P4 currently
reports version 9; Feather reports version 4 for its DAC or version 6 for the
external generator. Older P4 reports are accepted for some slower tests, but
the 1 kHz test requires version 8 or 9. Changing duration alone needs no new upload.
Grounded-input noise requires P4 version 9; see [GROUNDED-NOISE.md](GROUNDED-NOISE.md).
Install its NumPy dependency before running the updated PC smoke suites.

## Connect and run

Use the existing DAC-to-ADC coax, A1-to-A2 jumper, ADC power and inter-board wiring.
The ADC input is CH0P, with CH0N grounded. A2 must observe the same signal wire.
Both boards need USB; the known assignments are Feather COM4 and P4 COM3.
Insert the exFAT SD card. The runner offers 30, 300 or 600 seconds. Each run
adds its own preallocated pool, preserving files from previous runs:

- 30 seconds: two 16-second files, stop when full.
- 300 or 600 seconds: five 16-second files, circular overwrite within that pool.

Each file has 49,152,000 payload bytes plus a standard 512-byte header. The
16-MiB ring and real recording backend are used throughout. Changeover follows
bytes, not elapsed time. Files retain their allocated sizes. Longer runs read back
only surviving generations, typically the final 64–80 seconds; earlier overwritten
samples cannot be checked afterward. Whole-run loss and receive counters must
still be clean. Normal production startup configuration is unchanged.

Close serial monitors and reset both boards before each run. From the repository root:

```powershell
.\test\runHardwareSmoke.ps1
```

Select 1 (30 seconds), 2 (300 seconds), or 3 (600 seconds). To skip the menu,
use `-Seconds 300`, for example. The script first checks that both boards identify
as the correct idle firmware.
It sets P4 UTC from the PC clock and waits for acknowledgement before
sending `@TEST <seconds>` to both boards. Both must acknowledge before `g`
starts the default 1,024,000-samples/second configuration. P4 prepares SD, then
permits Feather to start the ADC. Preparation and retained-file readback add time
beyond the selected recording duration. Progress prints every 15 seconds. Preparation has a
180-second limit; Feather allows 185 seconds for readiness. No `p` is required.
Alternate ports can be supplied as `-Feather COM4 -P4 COM3`. Python needs pyserial,
as do the existing capture scripts. The runner never builds or uploads firmware.

Typical output (illustrative values):

```text
Digitizer -> P4 DMA: OK - no reported receive errors
Recording buffer: no overflow
SD written: 92,160,000 sample bytes; 1 file changes, 0 reuses
SD readback: retained-file checksums match (92,160,000 bytes)
DAC signal: 29 retained level changes verified
Hardware smoke test: 30 seconds
Recorded: 30.000 seconds
Signal: 1.480 / 1.750 V; 29 level changes verified
Largest plateau-average error: 2.1% of signal swing (limit: 10%)
PASS
```

Check confirmations print as the completed reports are validated, before the
voltage verdict. They are retained even if that final comparison fails. The byte
count excludes all 512-byte file headers. These are confirmed post-capture results,
not live claims about hardware still running.

A failure gives a plain-language reason and a nonzero process result. Complete
board logs and the result are retained under `captures/hardware-smoke/<timestamp>`.
During preparation the runner displays P4 milestones: Feather request received,
card mount, directory scan, each file allocation, and READY transmission. Every
15 seconds it prints elapsed waiting time and the last reported milestone. A
failure includes the board message and that milestone. Silence before the first
milestone does not establish whether the UART request arrived. These diagnostics
require the updated P4 image; Feather needs no additional update.

No card transfer to the PC is needed. If interrupted, the runner requests stop;
Feather stops after the selected duration; P4 has a fallback timeout five seconds longer. Keep the card installed
until writing/readback has finished. USB failure is not confirmation of a clean stop.

## What is checked

- The DAC alternates codes 110 and 135, one second at each level, starting low.
  These are commands, not assumed physical voltages. Feather measures the actual
  A2 voltage with a short 500-us burst every 100 ms throughout the run.
- File count, changeovers, reuse count and retained byte ranges must match the
  selected duration. Headers and surviving payloads are read in stream order.
- P4 records each decoded sample as three bytes. A checksum calculated from
  accepted source bytes for each surviving generation must match its disk readback.
  Acquisition faults, overflow, write errors, sync and close failures reject the run.
- Readback reconstructs signed samples across arbitrary read boundaries. At most
  801 bins of 100 ms are retained; Feather retains its latest 800 observations.
  Their original capture timestamps are preserved and their overlapping window is
  compared. The ADC whole-run count must match the selected duration.
- Every expected one-second level change in that window must appear within one
  100-ms bin of its expected position. A stuck or reversed signal cannot pass.
- Comparison uses six settled bins per second, from 0.2 to 0.8 seconds after
  each change. For each plateau, average its six disk bins and separately average its six
  A2 burst measurements. Compare those two voltages. Each retained plateau
  must pass independently; errors in different seconds cannot cancel. The
  largest plateau-average voltage difference must be at most 10% of the
  measured input swing (high minus low). For a 300-mV swing this is 30 mV.
- Input swing must be at least 150 mV. The fixed 10% limit is a provisional
  functional threshold, not a precision specification. It is not relaxed
  automatically after a failure.

## Measurement limits

Feather uses its SDK count-to-millivolt calibration table. A missing factory
calibration produces an explicit nominal-calibration note. ADS127 conversion
assumes the existing 2.5-V reference and unity input gain. There is no fitted
voltage gain, offset, or phase shift to make a recording pass. An inaccurate
reference or either ADC's calibration can cause a legitimate test failure.

The two ADCs have independent clocks and different averaging/filtering. Settling
margins make this a steady-level functional comparison, not a precision edge or
noise test. The recording duration tolerance is the greater of 20 ms or 100 parts per million
of the selected duration, allowing a final DMA tail and independent clock drift. Reported receive errors always fail, but
this tolerance and the hardware counters cannot prove that every ADC-generated
sample arrived. CRC also is not proof against every possible corruption.

Readback occurs on the mounted card, without a power cycle. PASS establishes
this selected-duration capture, not endurance, production qualification, or all SD cards.

## One input/output plot

Each complete, valid capture saves `comparison.html` in its capture folder, even
if the voltage comparison fails. It overlays the blue A2 input and orange SD
readback on one voltage scale over the retained window, using seconds since capture started. Each horizontal step is one settled plateau
average; vertical edges mark commanded one-second boundaries, not measured
transition timing or shape. No gain or
offset is fitted. Missing or malformed evidence does not produce a plot.

## Current storage format

Files use the standard `RINGSD02` header described in [the storage test](SD-CHOKE-TEST.md#file-header),
including UTC and text. The `file=` field in the console result now identifies a
logical `RECORD/session-N`, not a single filesystem path. Actual files are
`RECORD/Rnnnnnnn.BIN`; their headers identify the disk session and sequence.
Feather's run ID still ties the input and waveform reports together; it is not
the backend's persistent session ID. Old single-file `P4ADC03` readers do not
parse this format. The hardware smoke reader uses the backend on P4 and requires
no PC-side binary reader. Complete headers describe storage completion; the
final SAVED/PASS result additionally checks ADC integrity and analog comparison.

For manual console use after reset, set time before selecting a destination or
starting capture: send `@UTC <Unix-seconds>` followed by Enter and wait for `UTC OK`.
The smoke runner does this automatically. An unset clock refuses preparation.

## Serial-link diagnostic

Rebuild and upload both boards for the link-probe commands. From `espP4`, run:

```powershell
..\test\runHardwareSmoke.ps1 -LinkOnly
```

This skips the duration menu, ADC configuration and disk preparation. A numbered
probe must travel Feather -> P4 -> Feather within ten seconds of arming the
diagnostic. Feather reports bytes queued locally; this alone does not establish
transmission. P4 prints up to 128 received bytes in hexadecimal, rejected lines,
and polling/receive/error counters. Increasing polls confirm its control loop is
running. Matching replies on both boards establish the round trip. Saved logs
remain under `captures/hardware-smoke`. No waveform graph is produced.

Ordinary smoke runs now perform this probe before disk preparation. Their actual
preparation request also has a ten-second recognition limit; after P4 recognizes
it, the existing disk-preparation timeout applies. Idle firmware may report any
supported remembered duration (30, 300 or 600); that is not a version mismatch.
