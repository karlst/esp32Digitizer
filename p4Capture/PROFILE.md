# Optional 60-second CPU headroom test

Karl completed the PC checks, build/upload and 60-second hardware run on October 18.
See [results](../docs/STATUS-2026-10-19-P4.md). The objective is to
measure capacity for the Feather's current ADC-control duties, not future web
or networking features. Karl runs the following PC/build/upload steps; no board
operation is performed automatically by the assistant.

## What changes and what stays representative

`CONFIG_P4_CAPTURE_PROFILE` defaults off. When on, CMake compiles
`captureProfile.cpp`, retains the decoder/writer task handles, and selects the
shared 60-second run / 65-second receiver timeout. The normal configuration
retains 1800/1805-second timing. Capture pins, DMA allocation, private queues,
decoder, three-byte packing, 4 MiB ring, SD transfer sizes and 10 GB reservation
are unchanged. Synthetic t/u tests do not produce capture profiling reports.

The profiler runs from the existing Core 0 control task, not a new task. About
once a second it reads five persistent task runtime counters without stack
scanning, samples free internal RAM/PSRAM, and updates bounded totals. It adds
no work to the receive ISR or per-sample decoder loop and prints nothing during
capture. FreeRTOS runtime accounting still adds overhead at context switches.

`begin()` runs after receiver setup, before the READY reply. `finish()` freezes
measurements on entering stop, before DMA teardown and decoder draining. The
short READY/START handshake is included; SD preparation, final draining and
readback are excluded. Report printing and lifetime stack scans occur after
capture drains. Existing `TIMING`, `queueHigh`, and final SD `ringPeak`, write
latency, overflow/error/unsaved counters remain part of the evaluation.

## PC check, then separate firmware builds

From the repository in ordinary PowerShell, first run the portable math checks:

```powershell
.\p4Capture\test\runProfileChecks.ps1
```

This checks per-core normalization, weighted intervals, short final windows,
64-bit totals and 60/65-second timing. It does not validate hardware or RTOS
counter behavior. It does not run the long writer rollover test.

In an ESP-IDF PowerShell, from `C:\Users\karls\source\esp32Digitizer\p4Capture`,
build using a separate configuration and directory:

```powershell
idf.py -B build-profile -D SDKCONFIG=sdkconfig.profile -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.sd.defaults;sdkconfig.profile.defaults" build
```

The new defaults enable ESP timer microsecond runtime statistics and 64-bit
runtime counters. The profiler refuses to compile with incompatible settings.
If sdkconfig.profile already exists, defaults do not override saved choices;
use `idf.py -B build-profile -D SDKCONFIG=sdkconfig.profile menuconfig` to check
P4 capture diagnostics and FreeRTOS runtime settings. Do not change sdkconfig.sd.

After the build succeeds, upload P4 separately:

```powershell
idf.py -B build-profile -D SDKCONFIG=sdkconfig.profile -p COM3 flash monitor
```

Expect `sd-profile-60s`, `Profiling ON`, and `run=60 ... timeout=65`.
Feather MUST also use its existing **ads127-p4-capture-60s** environment. From
ordinary PowerShell, build and then upload as separate steps:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run --project-dir C:\Users\karls\source\esp32Digitizer\featherUi -e ads127-p4-capture-60s
```

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run --project-dir C:\Users\karls\source\esp32Digitizer\featherUi -e ads127-p4-capture-60s -t upload --upload-port COM4
```

Open the Feather console using its usual 115200-baud monitor. Verify the 60-second
banner; do not leave the 30-minute controller loaded for this test.

## Hardware run

Use the established powered wiring. On P4 press `p`, wait for `SD READY`, then
on Feather press `6`. Wait for the automatic stop, PROFILE summary, and final
`SD SAVED`/`SD FAILED`. Keep the card/power connected through file verification.
No file is deleted or reformatted. The successful earlier recording is preserved.

## How to interpret PROFILE

- `core busy/idle`: each core has its OWN 100% budget. Busy is elapsed time minus
  its idle task's scheduled time, covering system/driver work as well as the
  three named application tasks. Do not combine percentages by simple addition
  and interpret that as a percentage of one core.
- `worstBusy`: largest busy percentage in a measured interval of at least 0.5 s,
  normally about 1 s. A tiny final interval counts in averages but not this peak.
  `peakWindows=0` means no peak measurement. These are not microsecond worst cases.
- `task oneCore`: scheduled runtime for idle0, idle1, decode, sdWriter and control,
  expressed against one core's elapsed budget. A blocked SD write is not CPU
  execution throughout its wait. Other system tasks are not individually listed.
- `measuredUs` and `windowUs`: actual measurement coverage and interval lengths;
  delays are not disguised as on-time samples. `invalid` counts excluded snapshots
  or intervals; no valid intervals prints unavailable instead of apparent idle.
- `boundaryOvershoots`: task runtime sometimes exceeds an interval because task
  counters update on switches and snapshots are sequential. Percentages clamp
  at 100%; numerous/large anomalies need investigation, not a headroom claim.
- `snapshotMaxUs`: longest measured snapshot work (including heap queries and
  any preemption), not the total overhead of enabling runtime bookkeeping.
- `internalMinFree` / `psramMinFree`: lowest sampled free bytes during this run,
  not fragmentation measurements and not guaranteed minima between snapshots.
- `stackMinFreeBytes`: lifetime minimum unused task stack, sampled after capture.
  ESP-IDF reports bytes here. This does not measure the interrupt stack.

Interrupt time may be charged to the task that was interrupted, including idle;
these are scheduled-runtime estimates, not separately measured ISR execution.
Core averages alone cannot prove safety. Evaluate busiest windows, queue delays,
decoder throughput, SD stalls, memory, and all error counters together. A clean
baseline is evidence for planning the migration, not proof that new code is safe.

## Disable for the normal build

Return to **build-sd / sdkconfig.sd**, where `CONFIG_P4_CAPTURE_PROFILE` is absent
or disabled and FreeRTOS runtime statistics were already disabled. This removes
the profiler source, calls, retained task handles and its state; it also restores
30-minute timing. The original commissioning TIMING/error diagnostics remain.
Use the matching Feather 30-minute environment when returning to that build.

If disabling within the diagnostic config instead, also disable
`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` (and trace/formatting features if unused)
in menuconfig: removing the application option alone does not necessarily reset
SDK options saved in sdkconfig. Rebuild and verify the normal banner/config.

For an uninstrumented comparison at exactly 60 seconds, keep the current normal
firmware and stop it manually from Feather after about 60 seconds; do not claim
identical timed runs. An independently selectable automated 60-second baseline
is not implemented by this profiling option.
