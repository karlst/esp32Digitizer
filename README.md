# ESP32 Digitizer

ESP32-based 24-bit data acquisition system built around an ADS1256 ADC.

Target architecture:

```text
ADS1256 -> ESP32-S3 -> ring buffer -> microSD
                              -> decimated/status data -> ESP32 Feather -> browser UI
```

## Repository Layout

```text
esp32Digitizer/
    s3Recorder/     ESP32-S3 acquisition/recorder firmware
    featherUi/      ESP32 Feather UI/control firmware
    shared/         Shared headers, protocol definitions, version constants
    backups/        Local firmware/reference backups
    docs/           Project documentation
```

## Development Environment

Primary development environment:

- Windows 11
- Visual Studio Code
- PlatformIO
- Arduino framework
- Git / GitHub

Both firmware projects are PlatformIO projects.

## Branches

- `main` - stable branch
- `develop` - active development branch

Normal workflow:

```text
edit -> build -> upload -> test -> commit -> push
```

## Building

Open either PlatformIO project in VS Code:

```text
s3Recorder/
featherUi/
```

Then use:

```text
PlatformIO -> Project Tasks -> General -> Build
```

## Uploading

Connect the appropriate ESP32 over USB, then use:

```text
PlatformIO -> Project Tasks -> General -> Upload
```

For serial output:

```text
PlatformIO -> Project Tasks -> General -> Monitor
```

Both projects currently use a serial monitor rate of:

```text
115200 baud
```

## Acquisition timing and the 30k reader

The current program counts accepted samples and keeps the latest value. The ring
buffer and microSD recording in the target architecture above are future work.

Read `s3Recorder/src/acquisition.cpp` first for the execution sequence. Below
30,000 samples/second, the Data Ready interrupt wakes the core-0 acquisition task,
which reads three bytes through `ads1256.cpp`. At 30,000, `fastCapture.cpp` reads
those bytes immediately inside the interrupt, using the S3 SPI2 hardware. The
worker periodically copies its count/latest value and handles commands. It
detaches the interrupt before sending SPI commands, so they cannot collide with
a sample read. Communication and text formatting remain on core 1.

This is hardware SPI with a CPU wait, **not DMA**. A sample arrives every 33.3 us
at 30k. Bench measurements on September 30, 2026 found the old task wake cost up
to 17 us before the read even began; that left too little time. Also, a sample
already waiting after calibration had an unknown age. High-rate starts now wait
for a newly observed ready edge. The interrupt reader avoids the task wake cost,
uses instruction-RAM code, and bounds its hardware waits to 25 us. The whole ISR
has additional entry/exit overhead; 25 us is not a guaranteed physical-edge-to-exit
bound. Higher-priority interruptions can also lengthen elapsed time.

The stop command had a separate boundary problem. TI requires SDATAC to complete
while DRDY is low. Millisecond polling could find the end of that short interval.
High-rate Stop now briefly prevents task preemption, waits for a fresh falling
edge (bounded to 1 ms), and immediately sends the eight command bits. This fixed
the failure to start again after a successful 30k run.

USB timing fields reset on Start. `wakeUs` measures interrupt-entry-to-task-read
delay, not physical-edge latency. `spiUs` measures the library transfer call.
Both are zero at 30k because those stages are absent; SPI still takes time.
`readUs` and `maxReadUs` describe reader duration. `edges` counts handled ready
events, and `maxGapUs` is the largest interval between interrupt entries at 30k.
Read stage 4 means the SPI controller failed its deadline or was unexpectedly busy;
the program deselects the ADC without another SPI call and requires reboot.

The reader still rejects a transfer if DRDY stays low or another ready edge is
latched during it. GPIO can merge events while interrupts are masked, so neither
a zero missed-edge counter nor a plausible rate proves that every physical
conversion was captured. `missedEdges` tracks the lower-rate task path; the 30k
path reports observed overlapping reads instead. No sample sequence numbers or
external logic-analyzer measurements are available to prove lossless capture.

Bench results on the connected S3/ADS1256: a 60-second command interval accepted
1,805,182 samples (the host Stop adds some timing overhead), with steady displayed
rates of 30,001–30,002/s, 18–19 us typical reads, 20 us maximum read, 38 us maximum
ISR-entry gap, and no reported read fault. A separate 35-second run, restart,
15k and 1k regression runs also passed. These are timing/count tests with the DAC
stopped, not waveform accuracy or future recording-throughput tests. At 30k the
CPU spends roughly half of core 0's time in these short reads, plus interrupt
overhead; this implementation is not a design for a future 1M-sample/s ADC.

Run desktop checks with `./s3Recorder/test/runNativeChecks.ps1`; add
`-LegacyStatus` to check the older UART format. They cover acquisition/restart,
signed byte decoding, overlap rejection, bounded controller failure, command
framing, status parsing, and reboot ordering. They do not replace board tests.

Default S3 builds send the version-3 status frame with diagnostic counters. While
the Feather still has the older version-2 parser installed, build/upload with
`pio run -d s3Recorder -e legacy-feather -t upload`. This selects the same sampling
code with the shorter status frame. Today's S3 upload used the equivalent
`S3_LEGACY_STATUS=1` flag to keep the deployed Feather working; no Feather upload
was performed. Use the default environment once Feather has the version-3 parser.

## Shared Code

Common code is stored under:

```text
shared/
```

For example:

```text
shared/version.hpp
```

Both PlatformIO projects include the shared directory through their `build_flags`.

## Current Version

```text
0.1.0
```
