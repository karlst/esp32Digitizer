# ESP32 Digitizer

ESP32-based 24-bit data acquisition system built around an ADS1256 ADC.

For the current SD backend and future DMA integration, start with
[the backend reading guide](s3Recorder/src/backend/README.md).
Hardware diagnostics and host runners are catalogued in
[the test guide](s3Recorder/test/README.md). The historical test notes below
describe the experiments that led to the current configuration.

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
The active card adapter uses the Adafruit 4682 breakout and the S3 native
four-bit SDMMC controller at a requested 20 MHz. Connect **3.3V to 3V**, GND to
GND, GPIO5 to CLK, GPIO7 to CMD/SI, GPIO6 to D0/SO, GPIO15 to D1, GPIO16 to
DAT2, and GPIO4 to D3/CS. Leave DET unconnected. This is different from the
previous 5V SPI breakout; do not apply 5V to the new board. ADC and UART wiring
remain unchanged. Remove power when changing connections.

`sdmmcBlockDevice.*` transfers filesystem sectors using ESP-IDF's native SDMMC
controller. `FsVolume` retains SdFat's FAT/exFAT support, so existing formatting
and files remain usable. Only the disk task touches this adapter; the reusable
ring buffer remains independent of the controller and filesystem. An aligned
4096-byte scratch buffer handles buffers that are unsuitable for hardware DMA.

The opt-in `storage-check` build writes a new 64-KiB file at startup, closes it,
reopens it, and verifies every byte. It retains `/sdio_check_NNNNNN.bin` for
inspection and never replaces an existing file. Look for `SD full readback`
with `result=PASS`; successful compilation/upload alone does not verify wiring.
Two diagnostic environments retain the same check: `storage-check-slow` uses
four data wires at 4 MHz; `storage-check-onebit` uses D0 alone at 4 MHz. These
isolate communication faults; neither is the normal speed-test configuration.
The default remains four-bit 20 MHz. Switching test builds does not format the
card or change the reusable writer.
When changing from SPI to SDMMC, remove card power before the first SDMMC boot;
a processor-only reset can leave the card in its previous communication mode.

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

## Recording on a card prepared by a computer

Card filling and fragmentation are performed on the computer. The temporary
on-board preparation code and its build environments have been removed.
Use the normal `esp32-s3-devkitc-1` build. With Record to SD checked, Start
Acquisition opens a new recording and starts collecting samples immediately;
there is no filler creation or automatic deletion. Existing recordings and
computer-created filler files remain in place. P3 continues to report card
fullness, ring-buffer occupancy/peak/positions, bytes written, and maximum
write/flush delay for the recording.

## Temporary Choke test

Build the S3 environment `choke-test` for this experiment. The default
`esp32-s3-devkitc-1` build remains the normal digitizer recorder. The Choke build
starts stopped and does not initialize or read the digitizer.

- **Start Acquisition** opens a new file, then generates dummy 24-bit values stored
  in 32-bit words. Start always records, regardless of the Record to SD checkbox.
  The sample-rate dropdown is ignored; its value is echoed only for compatibility
  with the existing Feather command acknowledgement.
- Start at **750 kbps** of stored words, then add **250 kbps every 10 seconds**:
  750, 1000, 1250, 1500, and so on. These are decimal kilobits/s, including the
  fourth byte per sample. 24 Mbps would be reached after 15 minutes 30 seconds.
- **Stop** stops the source, drains queued data, and closes the file. A ring
  overflow or returned write failure also stops the source and attempts to save
  earlier data, marking the final file incomplete. No filling, formatting, or
  automatic deletion takes place. Existing recordings and PC fillers are kept.
- Each new Start resets the ramp. This is a temporary test firmware, not a
  persistent card-preparation operation; no completion marker is involved.

The synthetic source runs on core 0; the existing disk writer still pumps the
64-KiB ring on core 1. It uses the existing 4096-byte write batches, scheduler
delays, and the native four-bit SDMMC adapter. Free-space scans run only before
and after recording. This measures the complete current pipeline, not the card's
isolated maximum speed. The known completely-full-card hang is deliberately not fixed here,
per Karl's instruction; prepare free space on the PC before this test.

P3 already displays actual **write speed in bytes/s**, along with ring occupancy,
peak, bytes written, maximum write/flush delay and errors. In this build the last
measured write-speed window is retained after stopping. Multiply bytes/s by eight
for bits/s: 93,750 bytes/s = 750 kbps, and 3,000,000 bytes/s = 24 Mbps. No Feather
firmware or browser-file update is required. Its existing ADC labels refer to
the synthetic source in this temporary build; they do not confirm ADC hardware.

USB at 115200 adds a `CHOKE` line about once per second, with target, actual
generated and actual written kbps, last completed ten-second stage, elapsed
time, and result. `storage-limit` means ring/write failure (inspect P3 counters);
`producer-behind` means the generator itself fell more than 4096 words behind
schedule, so it stops instead of falsely blaming the card for a catch-up burst.
`rate-limit` is a guard against numeric overflow after an unrealistically long
run. Completed stages are ten-second observations, not endurance guarantees.

Files use distinct `S3CHK001` magic and describe the ramp in their headers; see
`shared/recordingFileFormat.md`. Start reading the implementation in
`acquisitionChoke.cpp`, then `chokeTest.cpp` for pacing and encoding. Detailed
comments explain the task boundaries and which counters measure accepted versus
written data. The generic `bufferedWriter` library is unchanged.

Desktop tests cover exact rate boundaries, fractional-word pacing, sample sign
encoding, ring overflow without throttling, write/flush failure, excessive source
lateness, Stop, duplicate commands, failed preparation, restart, and no ADC calls
in the Choke command path. Both firmware environments compile. Actual card speed
and physical Start/Stop behavior remain to be verified after upload is authorized.

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

### Choke test USB event log

The `choke-test` build also prints `EVENT` lines on USB. The acquisition and disk
threads queue numbers; the communications thread formats and transmits them.
No event writes to the card or waits for serial output on the sample thread.

Each line has a run number and `t` in milliseconds since that Start request,
**including file preparation**. `generator-start` marks when the ten-second ramp
actually starts. Bracketed field names match the comma-separated values in order.
Rates are decimal **bits/second**, including the full 32-bit sample word; buffer
occupancy is bytes; operation durations are microseconds.

- `rate-change`: previous/new target, live queued bytes, and last published write
  speed. That speed is a previous measurement window and can be stale during a
  long disk call; it is not an instantaneous measurement.
- `space-start` / `space-end`: free-space scan start, duration, buffer occupancy
  before/after, and free bytes. Scans now run only before/after recording on the disk task; their delay is
  separate from P3's maximum write/flush delay.
- `file-open`: preparation time and success, separate from sample writes.
- `slow-pump`: regular ring-to-card pumping taking at least 50 ms, with before/after
  occupancy and failure flag. Includes time the task was descheduled, not just
  electrical card transfer time. The timestamp is at completion; subtract duration
  to locate the beginning.
- `stop`: reason, target, last completed rate step, and queued bytes before draining.
- `drain-start` / `drain-end`, `file-close`: final saving measured separately.
  Writes inside the final drain are reported as one aggregate, not individual events.
- `final` / `final-buffer`: saved samples, lost samples, written bytes, peak and
  remaining buffer bytes, errors, and accepted samples. Reason codes: 0 idle,
  1 running, 2 user Stop, 3 storage limit, 4 generator late, 5 rate limit.

The 64-event queue reserves 16 slots from repetitive slow-pump messages for
transitions. `dropped` reports total diagnostic events omitted since boot if the
queue fills; it does **not** mean lost samples. Logging has small nonzero overhead.
Events are live USB output, not a persistent history: capture the serial output
while running the test to retain the timeline. Feather's interface is unchanged.

### Recording space and delay reporting (status version 5)

Free-space scans run before sample generation and after recording stops, never
in the active recording loop. P3 labels the live calculation **estimated free**;
a successful stopped-state scan changes it to **measured free**. Failed scans
leave **stale free** instead of falsely claiming a new measurement. Estimates
subtract newly allocated file-length blocks, rounding each part separately and
excluding repeated header writes. Directory growth/preallocation can differ.
No automatic low-space stop is implemented; write failure still stops recording.

P3 shows **Initial file opening** separately. **Maximum recording delay** resets
after initial opening and the baseline scan, and includes recording writes,
part rollover, final flushing and closing. The elapsed recording clock starts
at that same boundary. Initial opening includes filename selection/header sync.

Both S3 and Feather firmware, plus Feather LittleFS browser assets, require
updating for status version 5. New Feather still accepts old status versions;
old Feather does not accept version 5. USB SD lines also include `openUs`,
`spaceMode` (1 measured, 2 estimated, 3 stale), and `freeBytes` for bench checks.

### Active firmware: regular acquisition (Choke test parked)

The S3 default environment `esp32-s3-devkitc-1` reads the ADS1256 at the selected
sample rate. The Feather recording checkbox controls whether those real samples
are written to SD. Free-space scans remain outside active recording; the running
space display is estimated, with measured values before/after recording.

The synthetic test remains in `acquisitionChoke.cpp`, `chokeTest.*`, and
`chokeEvents.*`, selected only by explicitly building/uploading `choke-test`.
There is no automatic ramp in the regular firmware. Keep the test for validating
the connected SDIO breakout and SDMMC storage implementation. Last SPI result:
3.25 Mbps stage completed, overflow during 3.5 Mbps; no long-term rate guarantee.


### Isolated SDMMC read diagnostic

Build/upload `sd-read-diagnostic` to exclude every recorder source except
`sdCardDiagnostic.cpp`. It uses ESP-IDF sector reads directly: no ADC, UART1,
SdFat, recording adapter or ring buffer. Existing card sectors are only read;
there are no write, erase or format calls. USB serial remains at 115200 baud.
It runs once after boot; sending capital `R` repeats it. Feather monitoring will
show disconnected because this diagnostic deliberately does not run its link.

The test saves six 512-byte sectors in one-bit mode at 4 MHz, compares them in
four-bit mode at 400 kHz, 4 MHz and 20 MHz, then repeats one-bit mode. Hardware
CRC checks transfers; an exact byte comparison checks successful reads against
the baseline. A checksum in the log is only a compact data identifier. Failed
reads do not supply valid comparison data. This test does not test write speed.

2026-10-06 result: one-bit reads succeeded 4/4 before and after. At every four-bit
speed, sectors 0 and 65536 matched, sector 32768 reported invalid CRC, and the
following sector 32769 timed out. The latter may be recovery after the previous
failure, not an independent bad sector. Thus the failure persists without the
application storage/acquisition code; the electrical path, card and underlying
ESP-IDF SDMMC driver remain candidates. No Choke-test result is available yet.


The read diagnostic now also logs failed-transfer memory as **untrusted clues**.
Before each read it fills the destination with a position-dependent pattern.
After failure it counts altered bytes, compares bit positions against a valid
one-bit baseline, and emits both buffers as hex. Error-return buffers can be
partial or stale; bit positions must not be treated as a diagnosed physical wire.
The 2026-10-06 comparison found the first 120 bytes correct, followed by repeated
expected F4 bytes appearing as 4F and an untouched buffer tail. This does not
identify a particular bad pin or establish a driver defect. Raw results are in
`.pio/sdio-byte-clues.log` (local, ignored). No writes are performed by this test.

Direct-jumper follow-up (2026-10-06): the initial four-sector test passed, but
the recorder then failed CRC reading filesystem sectors 65654 and 66048 at
20 MHz. The diagnostic now includes those sectors. With the original SDK,
all six matched at 400 kHz and 4 MHz; the two added sectors failed at 20 MHz.
At 4 MHz, `storage-check-slow` passed complete 64-KiB file write/readback and a
short 30,000-sample/s recording saved 132,960 samples without loss or write
errors. This supports using 4 MHz for further testing; it does not certify
20 MHz. `choke-test-slow` is an explicit 4-MHz variant of the parked ramp test.

The 4-MHz Choke run then failed after 31.147 seconds at a 1.5-Mbps target:
`sdmmc_write_sectors` reported invalid CRC, followed by timeouts. The buffer
peaked at 32,904 of 65,536 bytes; this was a transfer failure, not buffer overflow
or an established throughput ceiling. The last complete ten-second stage was
1.25 Mbps, but even 4 MHz is not qualified for sustained recording. Log (local, ignored):
`s3Recorder/.pio/sdio-direct-choke-4mhz.log`. The failed recording is incomplete.

Expanded newer-driver comparison: ESP-IDF 5.5 passed the first six-sector run
and 17 repeats, then failed CRC on sector 65654 at 20 MHz on repeat 18. Returning
to the old driver without rewiring reproduced three 20-MHz sector failures.
The newer stack performed better here but did not fix the fault. No card writes
were performed during that driver comparison.

### One-bit versus four-bit write comparison

**Clock-label correction:** the historical original-SDK "4 MHz" results below
reported the requested limit, not active hardware dividers. IDF 4.4 initialization
falls back to 400 kHz for requests below 20 MHz. A later 10-MHz boot directly
confirmed that fallback in registers. Thus the earlier 1.25-Mbps completed stage
must NOT be treated as a measured 4-MHz throughput ceiling. This correction does
not invalidate the same-firmware before/after wiring comparisons. The separate
IDF 5.5 diagnostic uses a different driver and must not inherit this conclusion.

The recorder adapter now explicitly sets the clock after initialization and
verifies it from S3 hardware dividers before filesystem access. It compensates
the older driver's doubled intermediate-clock setting only when readback proves
that case, preserving the 100-ms data timeout. Unexpected clock results fail
startup. Register verification establishes configuration, not waveform quality.

Verified 10-MHz Choke result: initial register read showed 400 kHz despite the
10-MHz request; explicit setup then confirmed 10,000 kHz with four-bit transfers.
The ramp completed 3.25 Mbps and overflowed the ring at a 3.5-Mbps target after
112.875 seconds. No CRC/timeouts/write errors occurred. Peak ring use was
65,328/65,536 bytes; 109 offered words were rejected, while all 7,189,452 accepted
words drained. File header/size/tail checks passed, with 28,757,808 payload bytes.
Maximum disk latency was 138.155 ms. This is a limit observed with this buffer,
card and workload, not proof of the card's average bandwidth or sustained 3.25
Mbps endurance. Logs: `short-terminals-choke10-boot.log` and
`short-terminals-choke-10mhz.log` in `s3Recorder/.pio/`. Current S3 firmware is
`choke-test-10mhz`, stopped after overflow; earlier current-firmware statements
below are historical. No separate readback test preceded this ramp.

Both modes use the original recorder SDK, 4-MHz clock, the same direct wiring,
and the same synthetic sample sequence. `choke-test-onebit` keeps the standard
ramp but selects one-bit transfers. It filled the ring after 1.310 seconds at
750 kbps with zero write errors; accepted data drained and file checks passed.
That short run cannot establish communication reliability. Repeating the
four-bit ramp failed CRC after 31.145 seconds, again with 983,040 words saved,
on a different card sector from the previous failure.

For a sustained comparison, `sd-width-onebit` and `sd-width-fourbit` hold the
source at 250 kbps (31,250 bytes/s). A host Stop ends each run after approximately
150 seconds. These explicit diagnostic builds leave the standard ramp unchanged.
The one-bit run completed 150.711 seconds, saved 1,177,437 words, and reported
zero loss/write errors. Its file header, size and final word were checked after
close; this is not full-payload readback. Logs are local under `s3Recorder/.pio/`.

The matching four-bit fixed-rate run failed after 125.984 seconds, again with
exactly 983,040 words saved (3.75 MiB of sample payload). The failing write was
sector 86785, count 8, invalid CRC; later metadata writes timed out. Peak ring
use was only 6,408/65,536 bytes. Thus reducing the source rate changed the time
to failure, not the saved sample count. All three four-bit write failures used
different card sectors but stopped at the same position in the synthetic sample
sequence. One-bit crossed that position successfully. This suggests a four-bit
transfer/data-pattern issue; it does not yet distinguish electrical timing from
controller/driver behavior or a software boundary. No bad-sector mapping is
justified by these results.

Current S3 firmware is `sd-width-fourbit`, stopped after failure; Start uses the
fixed 250-kbps diagnostic, not the ordinary Choke ramp or ADC acquisition. The
incomplete test files are retained. The eight native check suites pass, including
fixed-source timing/content and the unchanged normal ramp. Detailed logs:
`fixed-width1-4mhz.log`, `fixed-width4-4mhz.log`, `choke-width1-4mhz.log`, and
`choke-width4-4mhz.log` under `s3Recorder/.pio/` (local, ignored).

Shortened screw-terminal wiring follow-up: with the SAME installed four-bit
fixed-rate firmware (no upload or code change), the 4-MHz/250-kbps test completed
150.698 seconds and saved 1,177,335 words (4,709,340 payload bytes), passing the
previous 983,040-word failure point. No transfer errors or sample loss occurred;
the ring drained and header/size/tail checks passed. Maximum reported disk delay
was 297.499 ms. Logs: `short-terminals-width4-boot.log` and
`short-terminals-width4-4mhz.log` in `s3Recorder/.pio/`. This supports the wiring
change improving communication but does not yet qualify higher clock/rate or
long-term endurance. S3 remains in `sd-width-fourbit`, now stopped after a
successful run; Start still selects the fixed 250-kbps diagnostic.

Choke ramp with shortened screw-terminal wiring: four-bit/4 MHz completed the
1.25-Mbps stage, then filled the ring at a 1.5-Mbps target after 32.653 seconds.
There were no CRC/timeout or write errors. Peak ring use was 65,420/65,536 bytes;
47 newly offered words were rejected. All 1,061,859 accepted words drained,
and file header/size/tail checks passed. Last reported write rate was 1.325 Mbps;
maximum disk operation delay was 28.013 ms. This run crossed the former CRC
failure point and ended on buffer capacity instead. It measures this recorder
implementation at 4 MHz, not the card's independent maximum throughput.
Logs: `short-terminals-choke-boot.log` and `short-terminals-choke-4mhz.log` in
`s3Recorder/.pio/`. Current firmware is `choke-test-slow`, stopped after overflow;
Start now runs the ordinary ramp, not the fixed-rate comparison. 20 MHz remains
untested with the shortened wiring.

20-MHz shortened-wiring follow-up: `storage-check` passed complete 64-KiB
write/readback (`/sdio_check_000007.bin`). However, `choke-test` failed on its
first 8-sector (4096-byte) sample write, sector 97537, invalid CRC. Generation
stopped after 45 ms at the initial 750-kbps target, with zero sample words saved
and only the 512-byte header reported written. Ring peak was 4312/65536 bytes;
this is a transfer failure, not throughput exhaustion. A subsequent operation
timed out. The small file check therefore did not qualify this workload at
20 MHz. Logs: `short-terminals-verify-20mhz.log`,
`short-terminals-choke20-boot.log`, `short-terminals-choke-20mhz.log` under
`s3Recorder/.pio/`. Current S3 firmware is `choke-test` (20 MHz), stopped after
failure. No card formatting or deletion was performed.

Improved-ground retry: register-verified four-bit 20 MHz still failed the first
4096-byte sample write (sector 154113, invalid CRC), after 46 ms at 750 kbps.
Only the header was saved; ring peak was 4312 bytes and later operations timed
out. This reproduced the previous first-block failure, with no demonstrated
improvement. The conditional 40-MHz test was not run because 20 MHz failed.
Logs: `improved-ground-choke20-boot.log` and `improved-ground-choke-20mhz.log`
under `s3Recorder/.pio/`. Current firmware is `choke-test` (verified 20 MHz),
stopped after failure. The 10-MHz results above remain the last CRC-free ramp.

### Fixed 4-KiB write timing, 2026-10-07

Uploaded standalone `sd-write-timing` to S3 COM4. Four-bit SDMMC, register-verified
10 MHz, existing exFAT. USB `R` starts 120 seconds of consecutive `file.write()`
calls appending the same deterministic 4096-byte block, without deliberate
between-write sleeps. No ADC, ring buffer, Feather link, free-space scan, or
preallocation. Normal recorder code is unchanged by this diagnostic.

Result: 53,028 full writes, 217,202,688 bytes in 120.000452 seconds, 14.480125 Mbps.
File-write latency: minimum 1.823 ms, maximum 139.727 ms, mean 2.256024 ms,
population standard deviation 2.671932 ms. Statistics include all successful
writes, filesystem work and driver waits; they do not isolate card busy time.
Opening took 48.920 ms; final sync succeeded in 2.961 ms (both outside the timed
writing window). No reported write errors; recorded file size matches accepted
bytes. No full payload readback was performed. Ten-second progress output is
outside per-write timings but included in elapsed throughput.

File `/write_timing_000001.bin` retained; existing files preserved. Full log:
`s3Recorder/.pio/write-timing-4k-10mhz.log`. Installed diagnostic is now idle;
Feather acquisition commands are unavailable in this isolated build. Only 4 KiB
was tested. This is a two-minute isolated benchmark, not proof of sustained
24-Mbps acquisition or a controlled comparison of individual recorder overheads.

### Matched 8/16-KiB write timing, 2026-10-07

User authorized two 60-second tests at 10 MHz with the SD adapter transfer limit
matching each file-write size. Added opt-in `sd-write-timing-8k` and
`sd-write-timing-16k`; default recorder adapter remains 4096 bytes. Both compiled,
uploaded on COM4 and completed with no reported write errors. Four-bit 10 MHz
verified from hardware dividers at boot. Existing exFAT/files preserved.

| Block / adapter limit | Duration (s) | Full writes | Bytes | Min ms | Max ms | Mean ms | Population stddev ms | Mbps |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 8 KiB | 60.001286 | 18651 | 152788992 | 2.451 | 139.699 | 3.210098 | 4.266597 | 20.371429 |
| 16 KiB | 60.004555 | 11695 | 191610880 | 4.122 | 148.822 | 5.123827 | 6.647925 | 25.546178 |

8-KiB file `/write_timing_000002.bin`: open 3.295 ms, final sync 2.983 ms.
16-KiB file `/write_timing_000003.bin`: open 3.285 ms, final sync 3.028 ms.
Both final syncs succeeded and file sizes matched accepted bytes; no full payload
readback. Timing includes filesystem allocation and card waits. Different files,
card locations and durations mean this is not a controlled endurance comparison.
16-KiB average exceeded 24 Mbps in this minute, but 148.822-ms stalls remain and
this isolated benchmark has no concurrent ADC capture or ring buffer.

Logs in `s3Recorder/.pio/write-timing-8k-10mhz.log` and
`write-timing-16k-10mhz.log`. S3 now has `sd-write-timing-16k` installed and idle;
COM4 closed. USB R repeats 16 KiB/60 seconds. No 2-KiB test, no code committed.

### Matched 32-KiB write timing, 2026-10-07

Authorized one-minute follow-up: `sd-write-timing-32k` uses 32768 bytes for both
file writes and SD adapter maximum transfers. Uploaded and tested on COM4,
four-bit register-verified 10 MHz, existing exFAT; no deliberate write sleeps.
Result: 6836 full writes, 224002048 bytes in 60.004857 s, 29.864522 Mbps.
Latency: min 7.666 ms, max 52.749 ms, mean 8.770758 ms, population standard
 deviation 1.557096 ms. Opening 3.303 ms, final sync 2.948 ms, both separate.
No reported write errors, sync succeeded, file size matched; no full readback.
Retained `/write_timing_000004.bin`; no existing files changed or deleted.
Log: `s3Recorder/.pio/write-timing-32k-10mhz.log`. S3 now idle with this diagnostic
installed; USB R repeats 32 KiB/60 seconds. COM4 closed. No normal recorder change.
This one-minute average is 24.4% above 24 Mbps, but below 32 Mbps. The lower
observed maximum delay does not guarantee future writes cannot pause longer;
card locations/state and finite test duration differ between runs.

### Matched 64-KiB write timing, 2026-10-07

Authorized one-minute `sd-write-timing-64k` follow-up: file-write size and SD
adapter transfer limit both 65536 bytes. COM4 upload succeeded, four-bit actual
10 MHz verified from hardware dividers, existing exFAT. 3722 full writes,
243924992 bytes in 60.012985 seconds, 32.516295 Mbps. Latency minimum 14.664 ms,
maximum 179.032 ms, mean 16.116688 ms, population standard deviation 10.648488 ms.
Opening 3.276 ms and final sync 2.658 ms measured separately. No reported write
errors; sync successful and file size matches, but no full payload readback.
Retained `/write_timing_000005.bin`; no formatting/deletion. Log:
`s3Recorder/.pio/write-timing-64k-10mhz.log`. Current firmware is this diagnostic,
idle; COM4 closed. R repeats the 64-KiB/60-second test.
Average was 8.9% above the 32-KiB run and 35.5% above 24 Mbps. It only marginally
exceeded 32 Mbps; concurrent capture remains untested. The 179-ms maximum shows
why the shorter observed maximum in the 32-KiB run was not a safe pause bound.
Normal recorder remains unchanged (default 4-KiB transfer buffer).

### PSRAM Choke configuration (2026-10-07; run in progress)

`choke-test-psram` enables octal PSRAM with Arduino `qio_opi`, keeps SDMMC at
verified 10 MHz/four-bit, allocates an owned 2-MiB ring through platform-supplied
allocation/free callbacks, and uses 64-KiB file writes/SD transfer chunks. The
normal build retains the internal 64-KiB ring for its interrupt-based ADC path.
PSRAM submissions are task-only; they must not be called from a cache-disabled
interrupt. Ring/control counters remain internal. Full blocks drain without a
mandatory scheduler tick between writes; idle/partial queues still sleep.

Startup verified 8388608 physical PSRAM bytes and write/read-tested the complete
2097152-byte ring; about 6288607 PSRAM bytes remained free at that point. No silent
fallback to internal RAM occurs if PSRAM setup fails. The physical-size API is
used because Arduino's heap-size report subtracts allocator overhead.

Nine native suites passed, including allocation failure, both wraparound copy
spans, concurrent ordering across multiple 2-MiB wraps, overflow, partial writes
and flush failure. Test-runner error checks now catch negative assertion exit
codes as well as positive errors. Firmware built/uploaded COM4. Logs are
`s3Recorder/.pio/psram-choke-boot.log` and `psram-choke-10mhz.log`.
The ramp remains 750 kbps initially, +250 kbps every ten seconds, counting actual
stored bits (four bytes per generated sample). Results will be appended below.

PSRAM Choke result (same run, now finished): completed the 25.5-Mbps ten-second
stage, then overflowed at a 25.75-Mbps target after 1008.532 seconds (16m48.5s).
Reason was storage-limit/ring overflow, not producerBehind or a card I/O error.
The final burst contained consecutive slow pumps of 177.504, 151.051, 285.795,
and 140.263 ms. The queue could not recover between them. Across the run, maximum
measured writer operation was 288.576 ms. An earlier burst at 13.5 Mbps peaked
at 1670788 queued bytes then recovered, demonstrating the value of the larger ring.

Peak occupancy 2096708/2097152 bytes. One refused 1024-byte submission means
256 generated 32-bit sample words lost; all 417021841 accepted words were saved.
Saved payload 1668087364 bytes; reported total with headers 1668089412 bytes.
Ring drained to zero, write errors zero. Both file parts passed header/size/tail
checks, not exhaustive payload readback. Final close succeeded in 4.058 ms.
Files belong to recording session 12 and are marked incomplete because of the
intentional overload test. All card files retained; no format/deletion/commit.

The PSRAM path passed 24 Mbps for its ten-second stage, but this does not prove
long-run 24-Mbps endurance: groups of slow writes, not only the longest single
write, determine required buffering. Current S3 firmware `choke-test-psram` is
stopped; COM4 released. Start repeats the unchanged ramp. Feather UI source was
not changed. Further buffer/rate tuning requires discussion/authorization.

### 4-MiB Choke, 20-Mbps start, run until failure (2026-10-07)

Updated `choke-test-psram` to a 4194304-byte owned PSRAM ring and initial
20000000 stored bits/s. Kept 64-KiB writer/adapter blocks, verified 10 MHz/four-bit,
and +250 kbps every ten seconds. The configurable initial-rate arithmetic now
uses initialBps/250000 instead of a hardcoded starting multiplier. Default
750-kbps and fixed-250-kbps tests remain unchanged. All ten native suites pass,
including a new exact-count/content/boundary test at 20 Mbps and the external
writer tests at 4 MiB. Firmware built and uploaded on COM4.

Boot verified physical PSRAM 8388608 bytes; ring4194304; remaining4191455;
whole-ring startup write/read check passed. Started at20Mbps and ran past24,
with no host time/rate cutoff, until automatic storage-limit at26.5Mbps.
Last complete ten-second stage26.25Mbps. Elapsed268.053s (4m28.053s).
Peak4193380/4194304bytes; lost256 generated words (one1024-byte submission).
Card/write errors0; all194559769 accepted words drained; finalring0.
Payload778239076bytes; totalwithheaders778240100bytes. Longest measured writer
operation282.771ms. File-close3.862ms successful. Session13/part1 retained,
marked incomplete due to overflow; header/size/tail checked OK, not full readback.

Failure was a cluster of slow writes, approximately168,104,256,79,280,151,194,
151,283ms. Backlog grew from about66KiB to full over about1.4seconds before Stop;
last in-flight write completed after production stopped. No producerBehind,
CRC or I/O fault. Different run/card positions mean the small improvement over
2MiB does not isolate buffer size as the only variable or prove24Mbps endurance.

Logs: `s3Recorder/.pio/psram4-choke-boot.log`, `psram4-choke-20start.log`.
Current S3 firmware `choke-test-psram` STOPPED; Start repeats4MiB/20Mbps ramp.
COM4 released. Existing files preserved; no formatting/deletion/commit.

### 128-KiB Choke, 4-MiB ring, 24-Mbps start (2026-10-07)

User-authorized change: `choke-test-psram` now uses 131072-byte file writes and
SD adapter maximum transfers, retaining the 4194304-byte PSRAM ring and verified
four-bit 10 MHz. Initial rate24000000 stored bits/s, +250000 every10seconds,
run until failure. Ten native suites pass with the external-ring checks using
128-KiB batches and the pacing test checking explicit24Mbps sample counts.
Build/upload succeeded on COM4. Boot ring test passed; physical PSRAM8388608,
PSRAMfree4191455; internal free229404bytes before worker startup. Static RAM
build report157588bytes. No PSRAM fallback or memory-allocation failure.

Result: last complete ten-second stage32.5Mbps; storage-limit at32.75Mbps after
355.044s (5m55.044s). Card write errors0. Ring peak4193476/4194304bytes;
one1024-byte submission rejected (256words). All314146590 accepted words saved,
ring drained0. Payload1256586360bytes; total1256588408includingheaders.
Longest measured writer operation298.455ms (earlier in run); final close3.904ms.
Session14parts1/2 retained incomplete, header/size/tail readbackOK; no full scan.

The FINAL failure differed from earlier bursts: measured write throughput stayed
around31.6Mbps while incoming rate rose above it. Queued bytes at~320,330,340,350s
were508416,1180216,2081692,3355504, then overflow. Completing a32.5Mbps stage only
means the buffer absorbed that stage; it is NOT sustained32.5Mbps disk throughput.
An earlier slow burst peaked2395600bytes and recovered. This supports further
fixed24Mbps endurance testing, not a guarantee of end-to-end ADC acquisition.

Logs `s3Recorder/.pio/psram4-block128-boot.log` and
`psram4-block128-24start.log`. Current S3 firmwareSTOPPED; Start repeats this
128KiB/4MiB/24Mbps ramp. COM4closed. No cardfiles deleted/formatted; no commit.
