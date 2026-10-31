# P4 SD file-ring choke test

This test needs only the P4, its SD card and USB power/console. No Feather, ADC
or signal generator is used. It measures whether storage can keep up with
3,072,000 bytes/second, the byte rate of our normal 1,024,000 samples/second.
It does not test reception from the ADC.

The PC runner presents three choices: **fresh disk** (formats/erases), or
**add files** (preserves existing files), or **use existing / stop when full**. Selecting fresh authorizes erasure;
there is no second erase prompt. Uploading or resetting alone does not format.
All three tests use the reusable session backend. Normal ADC/smoke recording
now reaches that same backend through `SdRecorder`; its new integration needs
the separate thirty-second ADC hardware smoke run.

The disk task uses a 16-KiB stack for function calls, separate from the 16-MiB
recording ring. File-inspection handles contain 4-KiB sector caches and are kept
outside that stack. The final `Disk task minimum unused stack` line reports the
smallest unused stack space observed during this run, in bytes. These changes
address the preparation stack fault seen on the first hardware run of Test 3;
Karl's corrected Test 3 passed, with 12,096 bytes minimum unused stack.
If the board crashes or reboots during a test, the PC runner saves a short tail
of the crash output and exits with failure instead of waiting for its full timeout.

## What it does

1. Fresh: formats a single-partition exFAT volume with 128-KiB allocation units.
   This replaces the old partition map; it is not a secure erase of flash cells.
   Add-files: mounts without formatting and records the length and CRC32 checksum
   of every existing file, including its header and unused reserved tail. An empty
   card is rejected by this test: run fresh first to establish files to preserve.
2. Preallocates five files with 46,080,000 payload bytes each, plus a 512-byte
   header. This is 15 seconds per file; the pool holds 75 seconds of input.
   Add-files creates **five additional files**, and rotates only through those
   new files. Existing recordings and unrelated files are never selected for writing.
3. Generates numbered data for 300 seconds. Core 1 submits 30,720 bytes every
   10 milliseconds to `FileRecordingBackend`, which owns the real `BufferedWriter`
   and its 16-MiB PSRAM ring. The ring contains only payload; incoming batch
   boundaries do not break up the large disk writes.
4. Core 0 writes through 16-KiB internal scratch memory, as the recorder does.
   Ring writes are at most 128 KiB. At each file boundary it completes the header,
   synchronizes and closes, then opens the next existing file. No file is deleted,
   resized or reallocated during the run. After file 5 it overwrites file 1.
5. Drains the ring and reads all retained files back, comparing their headers
   and every numbered payload word with the expected contents.
6. Add-files also rereads every original file and requires the same path, length
   and complete-content CRC32. These checks happen outside the timed run. They
   detect accidental changes, not deliberate checksum collisions.

A full ring rejects the incoming batch and counts it as lost. Earlier queued
bytes remain untouched, and later batches can be accepted when space returns.
Actual write/flush errors stop the run. Capacity rollover is based on payload
bytes, not elapsed time. Losing input does not close a file. Accepted data
continues in the same file until its capacity is reached. After recording stops,
all retained headers receive the final session loss total. Exact gap positions
are not stored; a session with loss must not be treated as continuous time.

The first reuse occurs around 75 seconds. A no-loss five-minute run writes
921,600,000 payload bytes: four complete passes, 19 file changes, 15 reuses.
Only the latest five file generations survive for readback. These are storage-test
files, not normal ADC recordings; do not feed them to the waveform reader.

## Test 3: use existing / stop when full

Run this after a completed fresh/add-files test, so compatible used files exist.
Select **3** in the same runner (or pass `-Test existing`). No formatting occurs.

1. Checksum all current files, and verify that useExisting + stopWhenFull refuses
   the used pool. A mount error or missing compatible recordings is not a pass.
2. Setup only: preallocate five additional unused 15-second files, without
   opening a recording. Existing files are preserved.
3. Snapshot all filenames and lengths. Start a new session using only the unused
   allocations with useExisting + stopWhenFull. Check that preparation added or
   resized nothing and selected exactly five files.
4. Generate until the backend signals full: nominally 75 seconds and 230,400,000
   payload bytes, four file changes, zero reuses. A five-second timeout margin
   prevents hanging; reaching that timeout is not a successful stop.
5. Drain and verify payload/headers; verify filenames and sizes stayed unchanged.
   Require a second start to be refused now that this pool is also full.
6. Checksum the original files again and require unchanged contents.

PASS requires no loss, exact capacity saved, the full indication, four file
changes, matching readback and preservation. The setup allocation and before/after
checks are outside the roughly 75-second recording. With changed file count or
seconds-per-file, capacity and nominal time change together. This is a P4-only
storage test, not the ADC hardware smoke test.

## File header

The header remains 512 bytes so payload starts at an SD sector boundary.

| Byte offsets | Contents |
| --- | --- |
| 0 | Version `0x01` |
| 1–2 | Total header length, little-endian: `0x00 0x02` = 512 |
| 3–22 | UTC ASCII, e.g. `2026-10-21T12:34:56Z` (20 bytes) |
| 23–50 | `Larry Lytle loves Lightning!` (28 ASCII bytes) |
| 51–63 | Zero padding |
| 64-71 | Format marker `RINGSD02` |
| 72-79 | Session ID (one run), unsigned 64-bit |
| 80-87 | File generation within that session, unsigned 64-bit |
| 88-95 | Reserved payload capacity, bytes |
| 96-103 | Valid payload length, bytes |
| 104-111 | Saved-stream byte offset (excludes rejected input) |
| 112-119 | Reserved, zero |
| 120-127 | UTC seconds since 1970, for the same timestamp |
| 128-131 | Samples per second (1,024,000 in this test) |
| 132-135 | Bytes per sample (3) |
| 136-139 | Sample format: 2 = synthetic counter stream; 1 reserved for signed 24-bit ADC |
| 140-143 | State: 0 unused, 1 incomplete, 2 complete |
| 144-151 | Final session lost bytes (repeated in every retained file) |
| 152-155 | Loss total finalized: 1 yes, 0 unknown/not yet finalized |
| 156-507 | Zero padding |
| 508-511 | CRC32 of bytes 0-507 |

All multibyte numeric fields are little-endian. Files are named
`RECORD/R0000000.BIN`, `R0000001.BIN`, etc. No catalog is stored: preparation
scans recognized headers for the next session ID and available slots. Earlier
`RINGSD01` and old choke headers are preserved by add-files but not selected by
use-existing. The new marker distinguishes loss-total metadata from the old
source-offset format. The requested version byte remains 0x01. Damaged headers
are not treated as reusable slots.

Neither ASCII field includes a null terminator or newline. The timestamp is
taken when that file generation opens, including on reuse. Finalizing the header
preserves that timestamp. Readback checks every header byte against the saved
generation metadata, including UTC, text and padding, then checks the payload.

The P4 refuses to start until the PC supplies UTC. The runner uses the PC's clock;
accuracy is limited by that clock, whole-second resolution and serial delivery.
The clock cannot be changed during the test. Input pacing continues to use the
independent monotonic timer, so setting calendar time does not alter the rate.

## Build on the PC

First run the ordinary PC checks from the repository root. The existing writer
suite also compiles the session backend against an in-memory FatFs substitute.
It checks header bytes/checksum, rotation, add-files preservation, use-existing
selection, stop-when-full, overflow recovery without extra file changes, loss
headers, short writes and flush failures. A separate check uses 128-KiB disk
batches to detect fragmented or unaligned writes. This does not access hardware or prove physical card performance.

```powershell
.\test\runSmokeChecks.ps1
```

Then in an ESP-IDF 5.5.6 PowerShell terminal:

```powershell
Set-Location C:\Users\karls\source\esp32Digitizer\espP4
idf.py -B build/choke -D SDKCONFIG=sdkconfig.choke -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.sd.defaults;sdkconfig.choke.defaults" build
```

See [Building](BUILDING.md#start-directly-from-powershell) to activate that terminal.
The build above neither uploads firmware nor touches an SD card.

## Upload and run on P4

COM3 was the last P4 port; use its current port if Windows assigned another.
Close any other monitor using that port (Ctrl+] exits ESP-IDF monitor).
From the same P4 directory, upload without starting a monitor:

```powershell
idf.py -B build/choke -D SDKCONFIG=sdkconfig.choke -p COM3 flash
```

Then, still in `espP4`, run:

```powershell
..\test\runSdChoke.ps1 -P4 COM3
```

Choose **1** for fresh. After it finishes, reset P4, run the same command and
choose **2** for add-files. No rebuild/upload is needed between these two tests.
The runner checks idle version-5 choke firmware, sets UTC from the PC, and starts
the selected preparation, the unchanged five-minute run, and readback. It displays progress
and saves the full log to `captures/sd-choke/<UTC-date-time>/p4.txt`.
It uses the same Python/pyserial dependency as the hardware smoke runner; pass
`-Python <path>` if needed. It neither builds nor uploads firmware. Optional `-Test fresh`, `-Test addFiles`, or `-Test existing`
bypasses the selection menu. The default 900-second overall timeout includes old-file
checks; use `-Timeout <seconds>` if many earlier files make those reads take longer.
This does not change the five-minute acquisition duration.

Ctrl+C requests a graceful stop. Already queued bytes may still be saving after
the PC runner exits; keep the card installed. Stopping early fails the timed test.
Stop does not interrupt an SD operation already in progress, including formatting.

Only one run is allowed per reset. Reset, then invoke the runner for another run.
For manual console use, send `time <Unix-UTC-seconds>` followed by Enter, wait for
`CHOKE UTC OK`, then send `run fresh` or `run addFiles` followed by Enter; `s` requests stop and `h`
identifies firmware. Every command ends with Enter. The PC runner supplies those
line endings automatically and logs rejected-command details.
Upload the normal/smoke firmware again to return to ADC recording.

## Output and interpretation

Example progress format (illustrative, not a measured result):

```text
Elapsed 48s | File 4/5 | Buffer 12.0% | Peak 38.0% | Lost 0 bytes
```

The console reads snapshots once a second from a separate task. It does not run
inside a disk write. Current use can change between snapshots; the final peak
comes from the writer's high-water counter, including peaks between reports.

The final report shows scheduled, accepted, written and lost bytes; loss due to
ring rejection versus missed generator deadlines; peak buffer use; longest
individual payload write; longest file changeover; maximum buffered write/flush
time; file changes/reuses; new-file readback result; original-file preservation result; and PASS or FAIL.

Scheduled bytes are the amount due at the target rate, including missed generator
deadlines. Accepted bytes are those actually copied into the ring. Written bytes
exclude headers and include earlier generations subsequently overwritten.

PASS requires the complete timed input, no loss, all accepted bytes saved, and
matching retained files. Readback I/O failures also fail the run even if
the timed writing phase completed. Software generator delays count as loss separately from
ring overflow, so a busy CPU cannot accidentally make a reduced-rate run pass.
Rejected batches are not retried or hidden by replaying a backlog at high speed.
Numbering describes the accepted stream; missing input is recorded in counters,
and the final session loss total is repeated in retained headers. Payload word
numbering remains continuous across accepted bytes; it does not locate missing input.

The current 16-MiB ring holds about 5.46 seconds of input when initially empty;
the original 4-MiB ring held about 1.37 seconds. Rollover and
write delays consume that headroom. A pass on one card/run does not guarantee
every card, continuous ADC acquisition, long-term endurance or power-loss recovery.
Readback uses the mounted card, not a power cycle.

## Change the test settings

Defaults are together in `espP4/sdkconfig.choke.defaults`. They seed the saved
`sdkconfig.choke` only on the first configuration. For later changes run:

```powershell
idf.py -B build/choke -D SDKCONFIG=sdkconfig.choke menuconfig
```

Under **P4 capture diagnostics**, change file count, seconds per file, run seconds,
ring MiB, or once-per-second reporting. Valid ring sizes are 1, 2, 4, 8 or 16 MiB;
allocation fails clearly if insufficient PSRAM remains. Save, rebuild, and upload.
Keep the run long enough to reuse files: more than file count × seconds per file.
Turning off live reporting preserves phase/final messages for comparison runs.

## Source reading path

- `espP4/test/choke/main.cpp`: console commands and reporting schedule.
- `espP4/test/choke/SdChokeTest.cpp`: orchestration, generated input and reporting.
- `espP4/test/choke/PreservedFiles.cpp`: independent old-file checksum checks.
- `espP4/lib/fileRecorder/src/FileRecordingBackend.cpp`: session preparation,
  ring submission, byte-count file rotation, finalization and retained readback.
- `espP4/lib/fileRecorder/src/SessionFileHeader.cpp`: standard header codec.
- `espP4/main/src/P4RecordingDisk.cpp`: P4 SD pins, mount and explicit format.
- `espP4/lib/bufferedWriter/src/BufferedWriter.cpp`: ring copying and ownership.
- [Backend contract](../espP4/lib/fileRecorder/README.md): preparation/full policies,
  task ownership, statistics and limitations.

The earlier 16-MiB test with UTC/text headers passed on hardware: 921,600,000
payload bytes, no loss, no readback mismatches, 2.9% peak buffer occupancy,
19 file changes and 15 reuses. That result predates this backend refactor.
The first session-backend hardware run failed: 143,308,800 bytes lost, 100% peak,
3,461 file changes. Retained payload/header checks matched. The gap-triggered file
changes have been removed and payload-only buffering restored. The 16-MiB ring,
five-file pool and five-minute duration are unchanged. This correction needs
Karl's PC checks, build/upload and a fresh run before add-files testing.

Karl reports that the corrected fresh test passed at 2% peak occupancy and add-files
passed below 3%. The new third test and its preallocation-only API have not yet
been built/run. Run PC smoke checks, rebuild/upload, reset, then choose 3.
