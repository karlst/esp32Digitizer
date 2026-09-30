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

Default S3 builds send version-4 status with acquisition and recording counters. The
30k bench upload used `S3_LEGACY_STATUS=1` to work with the then-older Feather.
The Feather has now been uploaded with support for status versions 2, 3 and 4;
the default S3 environment is suitable for subsequent uploads. The optional
`legacy-feather` environment remains for genuinely older Feather installations.

## Recording and SD card

The recording-ready Feather firmware and LittleFS web files have been uploaded
on COM3. It adds the Record to SD checkbox, stopped-only Delete all recordings
with confirmation, and P3 recording telemetry. Existing acquisition diagnostics
remain in an expandable section. Numeric totals retain full 64-bit precision.
Browser tests exercise recording, Saving, cancellation/deletion, disconnection,
and desktop/mobile layouts. Native tests verify status 2/3/4 compatibility.

The S3 implementation now connects the checkbox and deletion command to storage.
The Adafruit MicroSD breakout uses its own SPI controller: 5V to 5V, GND to GND,
CLK to S3 GPIO5, DO to GPIO6, DI to GPIO7, CS to GPIO4. Leave the breakout's 3V
and CD pins unconnected. ADC and inter-board UART wiring remain as documented above.

Reading path through the implementation:

1. `acquisition.cpp` opens the recording before starting the ADC and stops the
   producer before draining/closing. Every accepted sample is submitted, including
   every 30k interrupt read, not just the periodically displayed latest sample.
2. `sampleFormatter.cpp` encodes signed 24-bit ADC values into little-endian int32.
3. `lib/bufferedWriter/src/` owns a 64-KiB single-producer/single-consumer ring,
   writes 4-KiB blocks, retains in-flight bytes, and measures write/flush delay.
   Its byte interface knows nothing about an ADC, Feather, pins, or file format.
4. `recordingService.cpp` runs filesystem work in a core-1 task; communications
   continues while core 0 waits for preparation/closing. During acquisition core
   0 only submits RAM bytes; it never waits for a file write.
5. `sdRecordingSink.cpp` uses pinned [SdFat 2.3.1](https://github.com/greiman/SdFat) to mount FAT16/FAT32 or exFAT
   without formatting and
   creates numbered files under `/recordings/`. It splits before 1 GiB, preserves
   old files, and deletes only recorder-named files on an explicit Delete command.

Stop shows Saving until the ring drains and the file closes. Ring overflow or
storage error stops acquisition and marks the recording incomplete. A dropped
Feather/browser connection does not stop recording. Maximum delay includes opening,
writing, rollover and final flush/close, and is preserved after Stop. Free-space
queries run periodically; their delays are reflected in ring occupancy rather than
classified as write/flush calls. The initial 64-KiB ring holds about 0.55 seconds
at 30,000 four-byte samples/second. It cannot cover arbitrarily long card stalls.

Files begin with a 512-byte versioned header; see `shared/recordingFileFormat.md`.
`bytesWritten` counts successful write calls including header rewrites, so it can
exceed the sum of file lengths. `samplesWritten` excludes headers and queued data.
Flush success is the filesystem's report, not a guarantee against sudden power loss.

Each closed part is reopened for a bounded check of its header, length and final
sample. This is not a full-file checksum; the small readback is included in
close-time delay. The adapter uses SdFat sync/close results to detect failures.

Desktop tests include recording-aware commands, failure acknowledgements, every
interrupt sample queued, concurrent ring wrap/order, full-buffer protection,
partial writes and failed flushes. The 64-GB card's original exFAT filesystem now mounts successfully; no
formatting was needed. A short 1k/s recording saved and passed header/size/tail
readback. A subsequent one-minute 30k/s test saved 1,794,989 samples (7,179,956 payload
bytes). Accepted acquisition count matched saved sample count; no reported ADC
faults, rejected buffer samples or write errors occurred. The ring peaked at
24,696/65,536 bytes, maximum measured write/close delay was 17,870 us, and maximum
ADC read time was 20 us. Stop drained the ring to zero and the file passed
header/length/final-word readback. The two bench recordings remain on the card.
This short test does not validate a nearly full card, 1-GiB rollover, all payload
bytes, power-loss recovery, or unseen/coalesced ADC edges.
The exact UART contract is in `shared/s3StatusProtocol.md`.

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
