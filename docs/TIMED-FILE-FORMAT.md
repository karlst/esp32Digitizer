# Timed recording format, version 1

For the full explanation of FSYNC alignment, time calculations, reader algorithm
and accuracy evidence, see the [sample timing design](SAMPLE-TIMING-DESIGN.md).

The format is `[header][tBlock][sBlock]... [zero tBlock]`. A tBlock describes
one complete PPS interval; its sBlock contains exactly `sCount` three-byte
samples. The file ends normally with 48 zero bytes at the next tBlock position.
There is no header rewrite, sample-number field, validity flag or per-block CRC.
Physical SD chunks remain a separate concern: their size is 128 KiB, and they
can contain parts of several logical records.

## Implementation status

`espP4/lib/timedRecording/src` contains the byte encoder, runtime configuration,
and sequential formatter/file-rotation logic. `TimedFileSink` is the submission
interface for asynchronous storage; accepting bytes is not proof of an SD write.
`TimedBufferedSink` now implements it using the existing byte ring and an ordered
file-boundary queue. `TimedFatStorage` mounts without formatting, preallocates
fresh file slots and supplies actual FatFs writes/readback. `TimedRecorder`
connects these components to hardware interval capture and separate formatting
and disk tasks, selected by `--timed-sd-test` in the bench firmware.

The live test passed on November 7 in capture `20261107-153348`: 59 complete
intervals, 60,415,138 samples and 181,248,550 serialized bytes verified by full
SD readback. Each interval waits in staging until its closing boundary and
samples are available. Boundaries use two consecutive hardware-captured FSYNC
edges shortly after PPS; no endpoint is invented by subtracting a nominal
sample period. UTC is synthetic. See the [day report](2026-11-07-TIMING-AND-RECORDING.md).

## Header: 256 bytes

All integers, including samples, are little-endian. Metadata is encoded field
by field; C++ struct padding is never written.

| Offset | Bytes | Field | Meaning |
| --- | ---: | --- | --- |
| 0 | 2 | fileId | ASCII `DG` |
| 2 | 2 | version | Unsigned 16-bit value 1 |
| 4 | 8 | utcFirst | UTC seconds since 1970 for the first interval's opening PPS |
| 12 | 4 | delayNs | Signed 32-bit input-to-FSYNC correction, initially 2553 ns |
| 16 | 4 | pairsPerFile | Configured maximum pairs, default 60 |
| 20 | 236 | reserved | All zero |

`utcFirst` is known before submitting the first retained interval. It is not
allocation time, ADC startup time or the instant a disk write happens. The
bench test supplies `946684800` (2000-01-01) as a synthetic origin; this does
not claim GPS synchronization. Production GPS association is separate work.

The version defines the fixed header/tBlock/sample sizes and nominal acquisition
configuration; they are not repeated as separate header fields. The measured
PPS tick difference supplies the actual tick-to-second conversion on readback.

## tBlock: 48 bytes

| Offset | Bytes | Field |
| --- | ---: | --- |
| 0 | 8 | tcFsFirst |
| 8 | 8 | tcFsLast |
| 16 | 8 | tcPpsFirst |
| 24 | 8 | tcPpsLast |
| 32 | 4 | sCount |
| 36 | 2 | nSec |
| 38 | 10 | reserved, zero |

Tick counts are unsigned 64-bit extended counts from the same timer origin.
`tcFsFirst`/`tcFsLast` describe the first/last sample actually in the following
sBlock. Karl approved placing block boundaries shortly after PPS, rather than
requiring the block's samples to lie strictly between PPS edges. At each boundary
two consecutive captured FSYNCs identify the preceding block's last sample and
the following block's first sample. No sample is shared or skipped. The saved
PPS ticks remain the actual second references; the last sample may follow the
closing PPS. The analog-input correction is applied on readback and can move a
sample's analog time into the preceding second.

`sCount >= 2` is required for endpoint interpolation. Each block requires
`tcPpsFirst <= tcFsFirst < tcPpsLast` and `tcFsFirst < tcFsLast`. Adjacent blocks share exactly
the same closing/opening PPS count, and their FSYNC endpoints increase. These
structural checks cannot independently detect every missing sample; acquisition
must stop and report any sample loss or invalid/missing PPS.

`nSec` begins at zero in each file and increments by one per pair. At file
rotation it resets to zero, and the next header's UTC advances by the number
of completed intervals. The two-byte field allows configurable limits up to
65,535 pairs per file without changing the format.

## sBlock and time calculation

Each sample uses the existing packer's low three bytes, least significant first:
`FF FF FF` is -1, `00 00 80` is -8,388,608, and `FF FF 7F` is +8,388,607.
The status byte remains excluded. Zero sample values are ordinary data, never
an end marker. A reader advances exactly `3 * sCount` bytes before reading the
next tBlock; it never searches for a marker in the samples.

For zero-based sample index `iSample`:

```text
dtcSample = (tcFsLast - tcFsFirst) / (sCount - 1)
tcSample  = tcFsFirst + iSample * dtcSample
time      = utcFirst + nSec
            + (tcSample - tcPpsFirst) / (tcPpsLast - tcPpsFirst)
            - delayNs / 1,000,000,000
```

The Python reader returns integer UTC seconds plus an exact fractional second.
This avoids losing sub-microsecond detail by converting a large epoch time to
a floating-point number. Exact arithmetic does not establish physical accuracy.

## Submission and rotation

`TimedRecordingConfig` defaults to 60 pairs/file, 10 file slots, circular
overwrite and a 2553-ns correction. `start` accepts another configuration while
idle, providing the hook for later serial/web UI controls. The storage adapter
must allocate/select only this session's files; unrelated files are not slots.

The single formatting task calls `beginBlock` only after the metadata is final,
then `submitSamples` in small batches until exactly `sCount` samples have been
accepted. A batch cannot cross an sBlock boundary. The sink copies data into
its owned buffering before returning and must neither wait for nor perform
disk I/O in these calls. The submitting task never borrows mutable ring pointers.

After the configured pair count, the formatter submits the end marker and queues
file closure. Only the next interval opens another file. It rotates slot indices
modulo file count when overwrite is enabled, otherwise reports full. Normal stop
can close a shorter file after a complete interval; stopping in an unfinished
sBlock is an error and does not append a normal completion marker.

The reader stops at the end marker, ignoring unused preallocated trailing space.
Without a checksum or crash-recovery protocol, the marker is a normal-completion
convention, not a guarantee against arbitrary corruption or sudden power loss.

## Desktop checks

Karl runs from `espP4`:

```powershell
.\test\runDecoderChecks.ps1
python .\test\timedReaderChecks.py
```

The C++ checks use the real encoder, formatter and existing sample packer, with
an in-memory sink for file reuse and injected failures. Python fixtures are
constructed independently and cover 110/93/103 samples, signed values, timer
wraps, exact sample times, preallocated tails, truncation and malformed metadata.
These checks do not run firmware or establish SD throughput.

## Buffered storage ownership

The submitting task calls `TimedBufferedSink::beginFile/submit/endFile`. These
calls copy bytes or publish a boundary; none calls the filesystem. The disk task
calls `pump` and, after submissions stop, `finish`. The existing `BufferedWriter`
still owns its byte ring and releases a span only after the disk write completes.

A separate bounded queue holds up to 32 file boundaries. Each descriptor contains
a starting offset, slot number and an eventual closing offset. Publishing its
head/end flag uses release/acquire atomics; 64-bit offsets are never read while
being changed on the other core. The disk task frees a descriptor only after its
file closes. Queue exhaustion is a fault, just like byte-ring overflow.

One ordinary chunk can contain the end of one file and the start of another.
`TimedBufferedSink::write` splits at those boundaries, closing the old file before
opening the next. FatFs writes still use 16-KiB internal scratch transfers within
the outer 128-KiB chunks. Closing and flushing durations are included in writer
statistics. No producer waits for card I/O or receives mutable ring pointers.

`TimedFatStorage::prepare` creates a new `TIMED/Snnnnnn` directory and preallocates
`F0000.DG`, `F0001.DG`, etc. Circular overwrite is confined to that new file pool.
It never formats or deletes anything, and a failed allocation can leave unused
new files. Capacity is supplied by the board adapter with drift headroom; the
storage layer refuses writes exceeding that allocation. Files retain their
allocated tail space rather than reallocating on each reuse.

`runDecoderChecks.ps1` additionally checks this complete format/ring/FatFs path
against an in-memory filesystem. Tests cover no submission-time disk activity,
chunk/file boundary splits, byte and descriptor wraparound, slot reuse, old-file
preservation, readback and injected open/write/close failures. The first format
and Python reader checks passed on Karl's PC; these newly added storage checks
passed on Karl's PC before board integration.

## Adjacent-edge boundary capture

The FSYNC diagnostic's `FsyncBoundaryCapture` reads the hardware capture register
until two consecutive captures are observed. A bounded critical section excludes
local interrupts, but other execution delays can still cause skipped edges.
The capture routine is placed in internal instruction RAM to avoid flash
instruction-fetch stalls within the read loop. This does not guarantee a bound
on peripheral-bus delays. Confirm placement in the firmware linker map after
building; the earlier failing build placed this routine in `.flash.text`.
After a skipped edge, the newer actual timestamp starts the next comparison;
the code never invents the missing predecessor. It performs at most 32 reads,
checks elapsed time after each read, and rejects a search lasting 20 microseconds
or more. These are rejection limits, not a guarantee against hardware stalls. DMA and
the other core continue; no per-FSYNC interrupt is installed.

`FsyncBoundaryMath` requires an actual spacing of 156 or 157 ticks and numbers
the pair from the existing checked anchor with the same nominal ratio/rounding
guard as the predictor. Only numbering uses the nominal ratio; both returned
timestamps are direct captures. The pair must follow PPS within 10 ms.

`PPS BOUNDARY` rows report both ticks and adjacent sample indices. The PC parser
checks every row, requires one pair per mapped PPS, and writes complete interval
metadata to result.json. Sixty captured PPS boundaries ordinarily supply 59
complete intervals; producing 60 intervals requires 61 boundary events. This
`--pps-sd-test` continues to use the old sample-only SD format. The separate
`--timed-sd-test` now connects these boundaries to version-1 file writing.
Missing PPS, failed pair capture or a lost numbering margin stops acquisition.

## Live timed-format bench test

The 2026-11-07 run in `captures/fsync-association/20261107-145319` captured all
60 adjacent boundaries, crossed two timer wraps and passed the complete
184,341,357-byte sample-only SD readback. Its boundary capture routine ran from
internal instruction RAM. This supplied the hardware basis for the new writer,
whose separate passing board test is recorded below.

`FsyncTimedTest` selects the new path only on console command `t`. Command `d`
retains the existing sample-only comparison. Reset before changing modes.
`TimedRecorder` creates its own tasks and buffers only when `t` is selected, so
the old and new 16-MiB write buffers are not both allocated.

Core 1 uses the existing sample packer and copies into `TimedIntervalBuffer`:
2,097,152 three-byte slots (6 MiB). Core 0 queues copies of actual PPS boundaries.
A Core 0 formatting task copies completed intervals to the existing 16-MiB
`TimedBufferedSink`. A separate Core 0 disk task pumps 128-KiB chunks and alone
owns filesystem calls. Formatting can run while the SD task waits for hardware.
Both tasks sleep one scheduler tick after each cycle to allow timing/control
to run. The staging queue covers about two seconds of samples, roughly
one second of which is required just to wait for the closing PPS. If delayed
storage exhausts staging, recording faults rather than overwriting samples.

The first timed-format board run (`20261107-150920`) formed one complete pair
without decoder drops, but the FSYNC numbering task went 10.234 ms between
observations and correctly exceeded its 10-ms guard. Unlike the passing ordinary
writer, the new priority-4 worker had yielded only every 16 iterations. It now
sleeps one scheduler tick after every iteration, matching the ordinary writer;
the numbering guard is unchanged. Subsequent runs exercised this correction.

The next run (`20261107-151239`) stayed within the numbering guard (maximum
observation gap 2.979 ms) but filled staging after roughly four seconds. Comparing
allocators found that the new adapter had omitted the passing writer's 64-byte
alignment and DMA allocation flag. Its SD scratch now uses the identical
`heap_caps_aligned_alloc` request. `TIMED WORK` reports formatting and disk-pump
elapsed totals after shutdown; they include task preemption and exclude deliberate
worker sleeps and final readback. Later runs exercised this correction; the
original log did not establish the actual allocation's alignment.

Run `20261107-151737` still filled staging: 6.09 seconds of disk-pump time and
1.13 seconds formatting in about nine seconds of capture. The combined worker
serialized those activities, leaving the 16-MiB ring almost empty (143,193-byte
peak). Formatting and disk pumping are now separate tasks, priorities 3 and 4
respectively on Core 0. The formatter releases a completion flag only after its
last submission; the disk task acquires it before draining/closing and reading
formatter statistics. No buffer-size or timing-limit increase was made. This
task separation was exercised in the subsequent runs below.

The split-task run `20261107-152907` used the write ring (1,794,352-byte peak)
without filling staging, but two raw DMA deliveries were dropped when all 16
receive buffers were occupied. Maximum queue age was 7.425 ms; numbering had
800 matching checks and no running mismatch/unsafe step. The compiled decoder
hot loop and word-completion routine were still in flash. They are now marked
for internal instruction RAM to remove that source of fetch stalls; no decoder
logic, receive-queue size or timing guard changed. Cache stalls are a hypothesis,
not established by those counters. The compiled image subsequently confirmed
internal instruction RAM placement for both functions and boundary capture.

Run `20261107-153348` then passed: 60 PPS boundaries, 59 complete pairs,
60,415,138 samples, and 181,248,550 serialized bytes verified by full SD CRC
readback (`0a79e9d4`). Staging peaked at 1,026,927 samples and the write ring at
1,818,316 bytes; maximum write/flush duration was 110,850 us. There were zero
write errors or unsaved bytes. Maximum numbering-observation gap was 9.039 ms
against the unchanged 10-ms guard; this is measured margin, not a worst-case
guarantee. Both timer wraps and the stopped endpoint check passed.

The one-minute test preallocates one new file under `TIMED/Snnnnnn`, uses 60
pairs per file, and never overwrites older sessions. It normally retains 59
complete intervals from 60 PPS boundaries. Startup before the opening boundary
and shutdown after the closing boundary are intentionally omitted. Production
defaults in `TimedRecordingConfig` remain 60 pairs, 10 slots and circular reuse;
this first hardware verification intentionally uses a non-reused single slot.

The test header assigns synthetic UTC `946684800` (2000-01-01) to its first
retained PPS. No GPS or UTC accuracy claim follows from this value. The worker
checks a CRC over every serialized header, tBlock, sample and end-marker byte
against full SD readback. The PC independently derives retained sample counts
and expected file length from the printed PPS boundary records. A hardware
PASS requires both timing checks and `TIMED SAVED` with matching counts.

From `espP4`, after the normal FSYNC build/upload and both board resets:

```powershell
python .\test\runFsyncAssociation.py --timed-sd-test --runs 1 --p4 COM3 --feather COM4
```

Keep the existing Rigol PPS and divided CH0 input wiring. Wait for final readback
completion before removing the card. The normal `timedReader.py` can subsequently
read the `.DG` file directly from a PC card reader; its zero marker excludes the
preallocated unused tail. Desktop checks additionally cover staging wrap,
startup/shutdown omission, variable delivery order, file rotation and faults.
