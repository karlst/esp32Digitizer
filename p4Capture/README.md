# P4 ADC capture and SD recording diagnostic

**Latest checkpoint: [October 17 overnight handoff](../docs/STATUS-2026-10-17-P4.md).
The 30-minute ADC-to-SD test PASSED.** Read that handoff before resuming work.

Status: initial revision passed Karl's PC tests and built/uploaded successfully.
Five-second bench runs were clean through 256k samples/s; 512k filled the
software queue and stopped with one dropped block. Timing-v2 also exhausted
the pool at 512k, measuring decoder throughput below the input requirement.
Grouped-v3 subsequently passed PC tests and five-second hardware runs through
1.024M samples/s. The **60-second memory-only run also completed**, receiving
61,439,431 words at an estimated 1,024,020.0 samples/s, with all reported error
counters zero. Decoder throughput was 12.792 MB/s against 8.192 MB/s required;
queue peak was 3/16. This does not establish that every generated sample arrived.
One earlier post-upload ARM received no READY; restarting P4 restored the link.
That startup handshake issue has not been diagnosed or claimed fixed.

**SD-v1 passed Karl's PC tests and built/uploaded after a callback-type fix.**
Both cards then failed mounting with FatFs error 13; both were confirmed exFAT.
SD-v2 mounted exFAT and preallocated successfully. The first combined ADC/SD
run stopped after about 7.9 seconds: 8,062,439 decoded words, zero reported
capture errors, recording-ring peak 4,194,300/4,194,304 bytes and one overflow.
It saved 32,243,708 accepted bytes; draining afterward did not recover the
rejected batch. Decoder throughput was 8.884 MB/s against 8.192 MB/s input.
SD-v3 passed Karl's PC checks and built/uploaded, but the first SD-only start
hit a stack protection fault in sdTest before its START message. The 4 KiB task
stack included a 2 KiB local batch and floating-point startup formatting.
SD-v3b moved that batch to the global object's internal RAM, reserved an 8 KiB
task stack, and used integer startup formatting. Karl built and uploaded it.
This crash produced no usable SD throughput result. Test rates, write sizes,
preallocation, pacing and the recording buffer are unchanged.
SD-only **26 Mbps passed for 60 seconds**: 195,000,000 bytes, ring peak 368,700,
zero overflow/errors/unsaved bytes, and every generated word verified.
Write-call throughput was 29.190 Mbps. **32.768 Mbps failed after 7.616 seconds**:
ring overflow, 31,194,468 accepted bytes saved, zero pattern errors, write-call
throughput 29.123 Mbps. These results concern the card currently installed;
do not assume it is the original S3 reference card.

**SD-v4 passed the combined 60-second ADC-to-SD test:** 61,439,431 samples,
184,318,293 bytes (exactly three/sample), matching readback CRC f75cf499,
zero reported capture/storage errors, overflow or unsaved bytes. Ring peak
318,413/4,194,304; max write/flush 59,943 us; write-call throughput 28.978 Mbps.
File: P4376247.BIN. This retains the capture-loss limitations described below.

**SD-v5 passed Karl's PC tests, both builds/uploads, and the 30-minute ADC-to-SD
hardware test.** Current installed firmware is the matched 30-minute pair.
Run 1844422070 saved 1,843,184,119 samples / 5,529,552,357 bytes to P4037876.BIN,
with matching full-file CRC32 34e6f7ed, zero reported capture/storage errors,
overflow or unsaved bytes. Ring peak was 355,717/4,194,304 bytes; maximum write/
flush 72,455 us; write-call throughput 29.029 Mbps. ADC stop was confirmed.
Earlier power-interrupted attempts failed and do not count toward this result.
DMA, packing, status checks, SD clock, ring and transfer sizes are unchanged.
Expected payload is 5,529,600,000 bytes (1,843,200,000 samples), approximately;
normal capture start/stop limitations still apply. No existing files are deleted.
The original Feather/S3 environment retains its five-second run duration.

This standalone ESP-IDF project replaces Hello World on the P4 when uploaded.
The goal is to investigate receiving the ADS127L18 stream through PARLIO and
DMA, up to **1,024,000 samples/second**. DMA means the peripheral transfers data
to memory without a software read for every bit. The P4 now copies decoded words
into a 4 MiB PSRAM recording ring, then writes the onboard SD card. It does not
format cards, overwrite existing files, drive a DAC, or configure the ADC.

## Wiring already confirmed

Numbers are GPIO labels, not positions on the header.

| Signal | P4 | Other end |
|---|---|---|
| DCLK, incoming bit clock | GPIO0 input | Translator A5, formerly S3 GPIO9 |
| FSYNC, incoming sample frame marker | GPIO1 input | Translator A6, formerly S3 GPIO10 |
| DOUT0, incoming serial sample data | GPIO2 input | Translator A7, formerly S3 GPIO11 |
| Command UART receive | GPIO4 input | Feather TX / GPIO8 |
| Command UART transmit | GPIO5 output | Feather RX / GPIO7 |
| Ground | GND | Common Feather/translator/ADC ground |

GPIO37/38 remain reserved for the P4 USB UART console. P4 is the Waveshare
ESP32-P4-Module-DEV-KIT, revision 1.3 silicon, 16 MB physical flash. Defaults
select support for silicon below revision 3.0 and 16 MB flash. The program
refuses to compile if those selections are lost. This SD revision requires PSRAM.

The onboard card uses four-bit SDMMC at an initial 20 MHz: CLK43, CMD44,
D0=39, D1=40, D2=41, D3=42. LDO4 supplies the SD GPIO voltage domain. No external
SD breakout wiring is needed. These are the Waveshare board's fixed SD pins.

P4 and Feather use separate USB power/data cables (previously COM3 and COM4).
The old shared 5 V jumper is removed. Translator VDDA is supplied by Feather
3.3 V; VDDB comes from the ADC assembly. The ADC has its separate 6 V input
supply. Common ground continuity was verified. S3 is removed from these links.
Keep DAC coax disconnected. Do not join the boards' 5 V or 3.3 V rails.

## First: test the decoder on the PC

In ordinary PowerShell, from the repository root:

```powershell
.\p4Capture\test\runDecoderChecks.ps1
```

This compiles the real decoder using Microsoft C++ and feeds it simulated wire
bits. It checks every packing phase and frame length, buffer splits, signed
values, restart, header faults, missing clocks and incomplete tails. New checks
compare every emitted recording word, including short batches and rejected-output
recovery. The script also runs the existing reusable-writer failure and ordering
tests with the P4's 4 MiB/128 KiB configuration. A new bounded-memory test moves
over 4 GiB through the real writer to exercise its position wrap; it creates no
large disk file. Header tests cover the 4 GiB boundary, expected 30-minute size
and 10 GB size. It neither
uses nor proves the ADC, USB link, P4 peripheral, or sustained capture speed.

## Build the P4 SD revision

Build and upload BOTH boards for this duration change. The separate 60-second
and original five-second Feather environments remain available unchanged.

In ordinary PowerShell, build the explicitly named Feather environment:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run --project-dir C:\Users\karls\source\esp32Digitizer\featherUi -e ads127-p4-capture-30min
```

The original `ads127-capture-control` environment retains five seconds for S3.
Only the new environment selects `ADS127_P4_ENDURANCE=1`. The P4 CMake component
selects the same option; `shared/ads127BenchTiming.h` supplies both durations.
The SD change adds output batching to the decoder, so rerun its PC tests first.

In **ESP-IDF PowerShell**, using the installed IDF 5.5.6:

```powershell
cd C:\Users\karls\source\esp32Digitizer\p4Capture
idf.py -B build-sd -D SDKCONFIG=sdkconfig.sd -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.sd.defaults" build
```

The project sets the target to esp32p4. This build creates a separate sdkconfig.sd
using both defaults files, enabling 200 MHz PSRAM and explicit external-memory
allocation. Merely adding a default would not override the older saved sdkconfig.
It does not use or modify p4HelloWorld's configuration.
Do not run `set-target` merely to build again, since that regenerates configuration.
Check the result before proceeding to upload. If an existing sdkconfig selected
the wrong settings, use `idf.py -B build-sd -D SDKCONFIG=sdkconfig.sd menuconfig`.

## Upload and bring up, one stage at a time

1. Leave the signal wiring in place. Keep **both P4 and Feather powered by USB**
   during upload; do not send a Feather start command. The ADC must be stopped.
   For the first upload the ADC 6 V supply was off; translator partial-power-down
   isolation allows that. It does not protect the direct P4/Feather UART link,
   which is why both boards stay powered. Close the P4 monitor with Ctrl+].
2. In the same IDF PowerShell/project directory,
   `idf.py -B build-sd -D SDKCONFIG=sdkconfig.sd -p COM3 flash monitor`
   builds if needed, uploads, then opens its console.
   Confirm `P4 capture IDLE (sd-v5-30min)` and
   `Bench run=1800 seconds; receiver timeout=1805 seconds from ARM.`
   This does not start the ADC.
3. Before capture, both boards and both translator supplies must be powered.
   Karl verified 6 V input, VCCA/VDDA about 3.3 V and VCCB/VDDB about 1.8 V
   before the initial runs. Do not assume software idle electrically disconnects
   a UART output; avoid leaving one linked board powered and the other off.
4. Upload the new matching Feather build. Close its COM4 monitor with Ctrl+C,
   keep both boards powered and ADC stopped, then use ordinary PowerShell:

   ```powershell
   & "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run --project-dir C:\Users\karls\source\esp32Digitizer\featherUi -e ads127-p4-capture-30min -t upload --upload-port COM4
   & "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" device monitor --port COM4 --baud 115200
   ```

   Keep the P4 monitor open. Feather help (`h`) must say **1800 seconds each**
   and a matching receiver deadline of **1805 seconds**. Neither monitor starts
   conversions. ARM2 does not negotiate duration, so verify both banners:
   an older build may stop at five or sixty seconds instead.
5. With an exFAT card inserted and capture idle, type **p in the P4 monitor**.
   Wait for **SD READY**. Preparation reserves exactly 10,000,000,000 bytes
   in a new uniquely named file. No format or deletion is attempted on failure.
   No card reformatting is required for exFAT. Then type **6 in the Feather
   monitor** for the 30-minute SD run at 1,024,000 samples/s. After capture,
   wait for file verification and SD SAVED/FAILED; readback adds several minutes.
   Verification progress prints every 256 MiB. Closing releases unused reservation.
6. Inspect both consoles before advancing. Feather `s` requests an early stop.
   Each later stage is a separate manual request; there is no automatic sweep.

| Feather key | Stage | Samples/second | DCLK | Clocks per sample |
|---|---:|---:|---:|---:|
| g | 0 | 16,000 | 4.096 MHz | 256 |
| 1 | 1 | 32,000 | 4.096 MHz | 128 |
| 2 | 2 | 64,000 | 4.096 MHz | 64 |
| 3 | 3 | 128,000 | 8.192 MHz | 64 |
| 4 | 4 | 256,000 | 16.384 MHz | 64 |
| 5 | 5 | 512,000 | 32.768 MHz | 64 |
| 6 | 6 | 1,024,000 | 32.768 MHz | 32 |

Feather controls ADC configuration and start/stop over its existing SPI link.
The P4 receives samples directly from the ADC translator, not from Feather UART.
The updated Feather diagnostics use the name receiver instead of S3;
ARM2/READY2/STOP/DONE is the same protocol used here with P4.
After each run, wait for **SD SAVED** or **SD FAILED** before another preparation.
Each new run requires **p**, then **SD READY**, then a Feather rate key. A Feather
ARM without a prepared file is refused before ADC start. Do not remove the card
while preparing, recording or verifying. Preparation does not start conversions.

## SD data and result meaning

### First isolate SD writing (SD-v3)

Use the same card throughout a comparison and identify which card was used.
The documented S3 reference was a SanDisk Extreme 64 GB V30 A2: preallocated
26 Mbps for 30 minutes, 5.85 GB, no reported losses or write errors. This does
not establish the replacement card's performance or the P4 implementation's.

After the PC checks, build and upload the P4 as above. Leave Feather/ADC stopped;
no Feather commands are needed. In **P4 COM3**, use these steps separately:

1. Type `p`; wait for the exact `SD READY` message.
2. Type `t` for 60 seconds at **26 Mbps** (3,250,000 bytes/s).
3. Wait for source COMPLETE/FAILED, then SD SAVED/FAILED and patternErrors.
4. Only after reviewing that result, `p` again, wait for SD READY, then `u`
   tests **32.768 Mbps** (4,096,000 bytes/s) for 60 seconds.

The console key `s` requests an early synthetic-test stop, which is a failed
full-duration test. Feather `s` is a separate ADC command. Starts are refused
without a prepared file or while recording/finalizing. Tests do not run at boot.

CPU1 generates known words in batches of up to 512 and submits through the same
ring/CRC path as capture. The CPU0 writer retains its 128 KiB blocks, 16 KiB
internal-RAM transfers, 20 MHz four-bit bus and task delays. Preallocation is
now 10 GB, shared with the ADC test, but t/u durations remain 60 seconds. The generator computes
cumulative due words from elapsed time, never waits for ring space, and fails
on rejection or more than 100 ms of scheduling lag. Successful totals are
48,750,000 words / 195,000,000 bytes at 26 Mbps and 61,440,000 words /
245,760,000 bytes at 32.768 Mbps. Final source lateness above 100 ms also fails.
This tolerance is scheduling allowance, not a precise hardware clock measurement.

Require source COMPLETE, SD SAVED, zero overflow/storage errors, exact expected
payload count and zero patternErrors. Readback checks every stored word against
its expected index as well as the CRC. Synthetic files use final magic `P4SYN01`
and stage 0xffffffff, with the same header layout; the rate field is generated
words/s. They contain no ADC samples. A source COMPLETE alone does not prove saving.

writeCallMbps measures bytes divided by time inside the write adapter, including
copying and card waits but excluding sleeps, preparation, final synchronization
and readback. It is not an end-to-end sustained throughput result. The paced
test verdict and ring peak establish whether the entire path kept up for this
minute. A one-minute pass is not equivalent to the earlier 30-minute endurance run.

The old ADC file stored four bytes per sample, requiring **32.768 Mbps**.
SD-v4 now stores three bytes per sample, requiring **24.576 Mbps** at 1.024 million
samples/s. The SD-only t/u tests still use their original four-byte format and rates.

### Project-local exFAT support

The older S3 recorder used SdFat. This standalone P4 uses ESP-IDF's bundled
FatFs, whose source contains exFAT but whose default configuration disables it.
`cmake/exfat.cmake` creates an overridden `fatfs` component inside the selected
build directory before component discovery. It copies the installed SDK source
with its license notices and enables `FF_FS_EXFAT=1`, `FF_USE_LFN=3` and
`FF_MAX_LFN=255` in the copied configuration only. Long filename support is
required internally by exFAT even though recording filenames use 8.3 names.
Temporary filename buffers use the heap, not the disk task's stack.
The override also maps a disabled volume-label option to numeric zero: the
exFAT directory reader uses it in a C expression, where an omitted Kconfig
identifier otherwise causes a compilation error. Label editing remains disabled
unless explicitly selected in configuration.

This intentionally overrides saved filename-related menuconfig choices and
works with the existing sdkconfig.sd. All FatFs code and its consumers must use
the same copied headers because exFAT changes file-object layouts and file sizes.
CMake checks component selection; the adapter checks configuration and 64-bit
FSIZE_t at compile time. The override is pinned to ESP-IDF 5.5.6 and FatFs
configuration revision 80286, so an SDK upgrade requires review. The SDK
installation and S3 implementation are not edited. SD-v5 separately extends
ADC duration and the file header; the 10 GB reservation now requires exFAT.

After updating an existing build, in ESP-IDF PowerShell from p4Capture:

```powershell
idf.py -B build-sd -D SDKCONFIG=sdkconfig.sd reconfigure build
```

Look for `P4 FatFs: exFAT enabled` during configuration. Build success is not
a card test: after upload, prepare the existing exFAT card with `p`, require
`SD READY`, and only then perform the capture/readback check. No
automatic format, deletion or replacement of an existing file is introduced.

### Recording pipeline

The reusable bufferedWriter library owns a 4 MiB PSRAM ring and releases memory
only after writes finish. CPU1 submits batches without disk waits. A CPU0 task
writes 128 KiB blocks through 16 KiB internal-RAM transfer buffers. Overflow,
partial writes and flush failures invalidate the recording and trigger a stop.
At full ADC rate, payload is 3.072 MB/s; the ring covers approximately 1.36 seconds
of a stalled card, not an indefinitely slow card.

`ADC127 DONE` acknowledges receiver shutdown promptly; disk draining and readback
follow separately. `SD SAVED` requires clean capture counters, all accepted bytes
written, successful synchronization and close, and a matching readback CRC32.
CRC is a checksum of accepted payload bytes. It checks the file against those
bytes, not ADC accuracy, undetected hardware loss or persistence across power loss.
The same-mounted-card readback is not a power-cycle test.

Files use unique `P4xxxxxx.BIN` names. The first 512 bytes contain an initial invalid
header, updated after verification. New ADC files contain `P4ADC03` plus a zero
byte at offset 0. Offsets 8, 12, 16, 20 and 24 are little-endian uint32 fields:
version (3), valid (0/1), run ID, stage, requested samples/s. Offset 28 is a
uint64 payload byte count; offset 36 is uint32 payload CRC32; offset 40 is
uint32 record width (3); offset 44 is uint64 stored sample count. Bytes 52–511
are zero. Each payload sample is signed 24-bit
little endian: decoded `0x08123456` becomes `56 34 12`, `0x08ffffff` becomes
`FF FF FF` (-1), and `0x08800000` becomes `00 00 80` (-8388608). Readers must
sign-extend bit 23. The status byte is checked by the decoder but omitted from
the file; header error counters/histograms remain visible on the console.
An SD SAVED ADC result requires exactly three accepted bytes per decoded sample.
Old `P4ADC01` files are four-byte records. `P4ADC02` has three-byte samples but
uses the older 32-bit size layout. Select the header layout by magic/version.
Synthetic `P4SYN01` files remain version 1 with four-byte generated words and
zeroes after byte 35. These formats differ from the production sample formatter.
A zero valid flag denotes unfinished/failed capture. Final close errors can occur
after the header was written, so also require the console's SD SAVED result.

Final reporting includes ring peak occupancy, maximum write/flush latency,
overflow, storage error and unsaved-byte counts. Preparation/verification time
is not included in maximum streaming write/flush latency. Decoder timing now
includes packing, batch copying and CRC computation; compare it with the memory-only
baseline, rather than assuming the earlier throughput still applies.

## What to look for

SD-v5 counter review: raw DMA and decoded-byte totals are uint64 (expected
14,745,600,000 bytes over 30 minutes); submitted/written bytes and timing totals
were already uint64. The ADC v3 header now stores payload bytes/sample count
as uint64. Valid sample/header counts fit uint32 (1,843,200,000 nominal); shared
compile-time checks include the 1805-second timeout and 10% rate margin.
DMA block/pause counts also fit. Ring head/tail intentionally remain atomic
uint32: unsigned subtraction and the power-of-two 4 MiB capacity preserve
occupied space across their 4 GiB wrap; they are not file-byte totals. Rejection
counts cannot grow for a full run because the first failure stops acquisition.
Feather's unsigned millisecond subtraction remains safe at this duration.
This review does not authorize extending beyond the fixed 30-minute test.

P4 reports `ADC127 DONE <run>` after the receiver has stopped and all queued
blocks have been decoded. **DONE is a stop acknowledgment, not a pass verdict.**

- `words`: complete received 32-bit words (8-bit header plus signed 24-bit sample).
- `bad`: headers inconsistent with active channel zero, new data, no error flag.
- `periodErrors`: FSYNC rising transitions not separated by the configured
  number of DCLKs. An edge is a change in a signal level.
- `short`: a new frame arrived before the previous word's 32 bits completed.
- `drops`: completed DMA blocks that could not enter the software buffer pool.
- `errors`: invalid callback size/data or detected UART error during the run.
- `rate`: estimate from received clock pairs between first and last DMA
  completion timestamps, divided by the configured clocks per sample.
- `queueHigh`: most blocks waiting for decoding, out of 16 available copies.
- `tailBits`: partial decoded word at the end of the delivered data.
- `raw`, `sample`: last word in hexadecimal and sign-extended sample value.
- Header histogram and first eight bad words are printed only after stopping.

Timing-v2 and later builds print two `TIMING` lines on the P4 console after stopping:

- `decodeMBps`: raw bytes decoded per microsecond of time inside `feed`,
  equivalent to decimal MB/s. It excludes queue work and deliberate pauses.
  Compare against `requiredRawMBps` (8.192 at stages 5 and 6). Below that
  requirement establishes insufficient decoder throughput; above it alone
  does not establish sufficient throughput for the entire pipeline.
- `decodeAvgUs` / `decodeMaxUs`: average/maximum elapsed time to decode a
  completed block. Descriptor sizes can differ; compare with care.
- `queueWaitMaxUs`: longest time from the DMA callback to the worker starting
  that block, including copying and scheduling delays.
- `callbackGapMaxUs`: longest observed interval between DMA callbacks. This
  can include input-clock gaps and interrupt latency; it is not a loss counter.
- `copyMaxUs`: longest callback-entry-to-copy-complete time, including obtaining
  a free block but excluding the subsequent ready-queue send.
- `pauses` / `pauseTotalUs`: number and actual elapsed time of deliberate worker
  sleeps. All elapsed timings can include preemption and instrumentation overhead.

Expect substantial nonzero word counts, approximately 1800 seconds at the
requested rate, and zero bad/period/short/drop/error counts. The usual header
is 08; power/modulator flag bits are preserved in the histogram and need review.
The internal-short analog input should be near zero but is not an exact pattern.
A zero-word run is a failure even if every error count is zero.

The decoder discards an initial partial frame and never searches for a header
or silently shifts bad data. Disable drops the unfinished DMA descriptor (less
than 4096 bytes, up to about 512 samples at the fastest stage). Starting/stopping
and filter latency also affect totals: this is not an exact timed sample
count test. A whole lost frame or changed payload bit may escape these checks.

The raw two-wire stream reaches 8.192 MB/s at 32.768 MHz, although one useful
header+sample word per conversion is only 4.096 MB/s at the highest rate.
Zero errors in this short test do **not** qualify sustained disk recording.

## Buffer ownership and failure behavior

CPU0 handles commands and installs the DMA interrupt. CPU1 decodes. The driver
uses a 32 KiB circular DMA allocation; its interrupt supplies completed
descriptors. The callback immediately copies each descriptor into one of sixteen
private 4096-byte blocks. A FreeRTOS queue passes that block's pointer to CPU1;
CPU1 returns it only after decoding. This avoids holding a pointer to storage
that DMA will reuse. No disk operation or serial printing runs in the decoder.

The worker requests a one-tick sleep at most once per 100 ms of processing so
the idle task can run, replacing the original sleep every sixteen blocks.
One tick is 1 ms; actual elapsed sleep depends on tick phase and scheduling.
Normal empty-queue waits also allow background work. Grouped-v3 examines four
bytes (sixteen DCLK instants) together. A mask checks every FSYNC bit and its
preceding level to locate rising edges, including edges at group boundaries.
With one rise, the data are split into an old-frame prefix and a new-frame
suffix; with none, up to sixteen data bits are appended together. Multiple
rises use the original bitwise handling. Final one-to-three-byte block tails
also use bitwise handling without reading past the delivered buffer.
This reduces repeated per-byte decisions even when FSYNC changes inside a
group, and handles the 32-clock frames needed at 1.024M samples/s.
All original header, frame period, short-word and tail checks remain enabled.
An inability to keep up increments drops and causes FAULT. A 65-second
receiver deadline from ARM causes TIMEOUT if STOP is lost. Feather independently
stops its normal run after 60 seconds and reacts to FAULT/TIMEOUT. A P4 fault
cannot physically stop the ADC if the command link is disconnected.

ESP-IDF 5.5.6 cyclic RX uses DMA descriptors without ownership checking and
does not supply a reliable hardware-overrun counter through this API. Our drop
counter covers the private software pool, not every possible peripheral/ISR
overrun. A delayed interrupt could miss data before the copy; framing and rate
checks improve detection but cannot prove losslessness. Independent known-data
and longer-duration testing are required before production claims.

## Reading path and references

- `main/main.cpp`: UART handshake, memory ownership, timeout and reporting.
- `main/frameDecoder.h` / `.cpp`: unpacking, FSYNC alignment and header checks.
- `test/decoderChecks.cpp`: desktop checks and explicit payload-check limitation.
- `../shared/ads127BenchRate.h`: rate table shared with unchanged Feather code.
- [TI ADS127L18 datasheet](https://www.ti.com/lit/ds/symlink/ads127l18.pdf),
  sections 7.4.10.3–7.4.10.6: header bits, FSYNC, rising-edge data capture.
- Installed IDF 5.5.6 `components/esp_driver_parlio/src/parlio_rx.c` and
  `components/hal/include/hal/parlio_types.h`: DMA behavior and pair packing.

## Initial bench results, October 17, 2026

These are the original firmware results, before timing-v2. Every run used the
ADC internal-short input. The clean runs reported ADC status 0x00 after stop.

| Stage | Requested samples/s | Received words | Observed samples/s | Queue peak | Result |
|---|---:|---:|---:|---:|---|
| 0 | 16,000 | 79,934 | 16,000.3 | 1/16 | All reported errors zero |
| 1 | 32,000 | 159,995 | 32,000.6 | 1/16 | All reported errors zero |
| 2 | 64,000 | 319,991 | 64,001.2 | 1/16 | All reported errors zero |
| 3 | 128,000 | 639,983 | 128,002.5 | 1/16 | All reported errors zero |
| 4 | 256,000 | 1,279,999 | 256,005.2 | 2/16 | All reported errors zero |
| 5 | 512,000 | 12,147 | 512,061.3 | 16/16 | FAULT, one dropped block |

Stage 5 had zero bad headers, period errors, short words and other reported
errors, but it stopped early after software-buffer exhaustion. Feather confirmed
ADC stop and status 0x00. This does not establish a P4 hardware speed limit.
The earlier pause and decoder costs were not timed, so their contributions
cannot be separated retrospectively. Timing-v2 was then tested with the
**same sixteen-block pool and 32 KiB DMA buffer**; results follow below.

No assistant-run build, upload or hardware test has been performed. Karl runs
the expanded reference-comparison tests and builds/uploads the revision.

## Timing-v2 results and grouped-v3 follow-up

Karl's PC tests and firmware build passed for timing-v2. Stage 4 remained clean:
1,279,999 words, 256,005.8 samples/s, queue peak 2/16, decode 7.716 MB/s versus
4.096 MB/s required. At stage 5 the run stopped early with one dropped block:
51,307 words, queue peak 15/16, decode 7.705 MB/s versus 8.192 MB/s required,
maximum queue wait 8,013 us. Header/period/short/other error counters were zero.
Only one deliberate pause (747 us) occurred; decoder throughput itself was
below the input requirement even when excluding pauses and queue operations.

Grouped-v3 changes only decoding, the version banner, tests and documentation.
It keeps the same timing instrumentation, scheduling, buffer capacities, pins
and Feather protocol for comparison. PC tests now cover every one of sixteen
edge positions in a group, four input address alignments, and fixed split sizes
from one byte through 4096 bytes, including frames with injected FSYNC faults.
Those tests cannot establish hardware throughput; the expected improvement
must still be measured. Retest 256k then 512k before attempting 1.024M.

Stages 5 and 6 both receive 8.192 MB/s of raw paired bits. Stage 6 completes
twice as many words and header checks, so passing stage 5 is not evidence of
sufficient stage-6 margin or readiness for simultaneous SD recording.

## Grouped-v3 five-second results and 60-second follow-up

Karl's expanded PC tests passed and grouped-v3 built/uploaded successfully.
Stage 4 stayed clean (decoder 18.656 MB/s). Stage 5 completed with 2,559,999
words, 512,010.2 samples/s, zero reported errors, queue peak 2/16, and decoder
18.694 MB/s. Stage 6 completed with 5,119,999 words, 1,024,020.2 samples/s,
zero reported errors, queue peak 3/16, and decoder 12.827 MB/s. ADC status after
stopping was 0x00. These establish clean short runs, not bit-perfect payload
verification or sustained SD recording.

The completed follow-up was **60 seconds, memory only**, at stage 6. Feather
stops after 60 seconds from confirmed START; P4 times out after 65 seconds from
ARM. The two-second READY and DONE waits remain unchanged. Manual `s` and fault
handling remain active. The software normally stays quiet during capture and
prints the summary after stopping. Expected order of magnitude is 61,440,000
samples; normal start/stop and unfinished-descriptor limitations still apply.

No decoder, buffer-capacity or scheduling change accompanied that longer run.
At 8.192 MB/s, 60 seconds is 491,520,000 raw bytes. Even the 65-second deadline
is only 532,480,000 bytes, within the 32-bit raw/decoded byte counters; sample,
frame, header, block and pause counters also fit. Clock and timing totals are
64-bit. Shared compile-time checks guard the byte-counter range and timeout
margin. The unsigned Feather millisecond subtraction remains safe across its
timer wrap for that duration. SD-v5's completed counter review is documented
above; the older raw-byte counters have now been widened for 30 minutes.

Karl built and uploaded both revisions and supplied the completed run log.
The assistant has not independently built, uploaded, or operated hardware.
The currently pending work is validation of the SD revision described above.
