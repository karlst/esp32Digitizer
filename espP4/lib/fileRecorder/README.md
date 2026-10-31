# Session file recorder

`FileRecordingBackend` holds incoming bytes in a ring buffer and writes them to a
selected list of preallocated files. FatFs is the filesystem library used to open,
read and write those files. The backend does not depend on an ADC, Feather, command
parser or particular reporter.

`SdRecorder` connects this backend to the P4 acquisition code. Core 1 packs and
submits ADC readings; a separate disk task on Core 0 prepares files, writes queued
bytes and checks each remaining file after recording stops. A task is a separately
scheduled software thread.

For a reading path, start with `RecordingSessionConfig.h`, then the preparation,
copyToRingBuffer, pump and finishRecording methods in `FileRecordingBackend.cpp`.
`SessionFileHeader` defines the disk header. `BufferedWriter` manages RAM;
`RetainedPayloadChecksums` checks submitted data against the later disk readback.

The storage-only tests and real ADC hardware runs have passed, including the 30
October 2026 external-generator runs at 0.5 Hz and 1 kHz. Those results cover the
tested settings and card, not every possible workload or power-loss case.

## Session parameters

A session is one run, beginning with `prepForRecording` and ending with
`finishRecording`. `RecordingSessionConfig` supplies:

- `_secondsPerFile`: sample-data capacity = seconds × sample rate × bytes per sample.
  The 512-byte header is additional. Rollover follows bytes, not a timer.
- `_fileCount`: number of files to allocate, **additional** files for addFiles.
- `_preparation`: `formatAndAllocate`, `addFiles`, or `useExisting`.
- `_fullPolicy`: `overwriteOldest` or `stopWhenFull`.
- `_sampleRate`, `_bytesPerSample`, `_sampleFormat`: caller-supplied recording
  metadata. The backend does not configure or interpret an ADC.

Fresh formatting is explicit. Failed mounting never triggers formatting. Formatting
or adding files selects only the newly allocated files for this run. The pool is that
selected list, not every file on the card. Add-files preserves all previous
recordings and unrelated files. Allocation failure leaves any new/partial files for
inspection; it does not delete old ones.

Use-existing ignores count/duration, scans compatible prepared headers, and selects
unused files first. With overwrite enabled it also selects older recordings, ordered
by session number and the sequence of writes within a session. With stop-when-full it
selects only unused slots and refuses preparation if none remain. File capacity stays
unchanged, including when the final recording uses only part of a file.

The managed directory is `RECORD`, with names `R0000000.BIN` onward. Session IDs
increase from the largest recognized header; formatting starts session numbering
again at 1. Filename numbers are not indices into the in-memory file list. There is
no separate catalog file. Unrecognized or damaged headers are never candidates for
reuse, and their names remain occupied. Scanning rejects directories above 4096
entries rather than silently ignoring files.

`preallocate` supports a separate setup step: use fresh/add preparation to reserve
unused files without opening a recording or starting the ring. It shares the same
allocation implementation as `prepForRecording`. Later call `prepForRecording` with
`_preparation = useExisting` to record into that prepared capacity. Never call either
while acquisition is active.

## Calls and ownership

Build all users with `BUFFERED_WRITER_EXTERNAL=1` and identical ring capacity/
block-size definitions. `initializeMemory` allocates the ring through caller
allocation/free functions and reserves temporary internal RAM for disk transfers. Do
this before acquisition. The caller supplies an elapsed-microsecond clock for
measuring write delays and a UTC calendar clock for file timestamps. The P4 adapter
selects the SD pins, card voltage, clock and explicit exFAT format.

One disk task calls `prepForRecording`, `pump`, `finishRecording`, `verify` and
`getStatistics`. One sample-processing task calls `copyToRingBuffer` and optionally
`skipInput`. Stop submitting samples before `finishRecording` or another
`prepForRecording`. Objects must outlive both tasks. On the P4, the ring lives in
PSRAM (external RAM). Submit from a normal task, not an interrupt handler: external
RAM may be unavailable during flash operations.

`prepForRecording` waits until setup finishes: true means files and ring are ready.
In contrast, `SdRecorder::prepare` only queues a request for its disk task.
`copyToRingBuffer` copies without allocating, accessing disk or waiting for a write.
Batches must contain whole samples and at most 65,520 bytes. It returns the number of
sample-data bytes accepted into RAM, not bytes written to disk. Ordinary overflow
drops the whole batch and continues; under stop-when-full a final prefix may be
accepted. The caller checks `getFailed` and `getFull` between submissions and
accounts for rejections.

The ring contains only sample data (called payload in the file format). Incoming
batch boundaries do not break up the 128-KiB disk batches. Each batch is copied in
pieces through a 16-KiB temporary internal-RAM buffer before writing to the card.
Only filling the current file triggers a file change. An SD sector is 512 bytes. When
file capacities are multiples of 512, normal writes stay on sector boundaries. Other
capacities and the final partly filled batch also work, but can require writing part
of a sector.

Overflow drops the incoming batch, counts the missing bytes, and keeps using the
current file when space returns. `skipInput` adds known missed input, such as missed
generator deadlines. Neither operation closes a file or writes a header. After
acquisition stops and the queued bytes have been written, `finishRecording` saves the
final session loss total in every retained header. The total is repeated, not divided
among files; do not sum it across files. Exact gap locations are not recorded. A
session with loss must not be interpreted as a continuous, evenly spaced time series.
An unfinalized loss flag means the total is unknown, not zero.

`pump` retains ring memory until the write finishes. Short writes, I/O errors and
flush failures stop the backend. If only the start of a batch was written, that part
is not submitted again. `finishRecording` writes remaining queued bytes and closes
the file, marking it complete only after successful storage operations. The caller
must still examine losses before claiming a loss-free session.

The disk task updates statistics. To report them from another task, arrange a
protected copy so the reader cannot see fields from different updates. `_written`
counts only payload, including data later overwritten during circular recording. The
ring and its `_buffer` counters also contain only payload. `_lostBytes` is set by
`finishRecording` after sample submission stops; it is not a live loss counter. The
choke test reports its own running loss total. Peak buffer use and the longest
write/flush time include file changes during capture. Updating the final loss totals
in headers happens afterwards and is not included in that time.

## Header and verification

[Header layout and hardware procedure](../../../docs/SD-CHOKE-TEST.md#file-header)
describe the 512-byte layout: timestamp, text, session and sequence numbers,
capacity, bytes written, loss totals, completion state and header checksum. The
timestamp records when the file opened, to whole seconds; it does not give the
precise time of its first ADC sample. Unwritten space is not sample data. After an
interrupted write, more data may be on disk than the last saved header reports; the
header does not claim that data is complete.

`verify` reads the files written by this session that are still present. It compares
their headers with the saved in-memory copies and can pass sample bytes to a
caller-supplied checking function, in original recording order. One overload also
passes a caller-supplied context pointer, allowing a recorder object to keep its own
checksum or waveform state without global variables.

The checking function must finish using the supplied buffer before returning; the
next disk read reuses it. Returning false rejects verification. Data already
overwritten cannot be checked, and files untouched by this session are not read.
`pump(true)` is a separate operation: it allows the caller to write a final batch
smaller than the normal block size before closing the recording. `PreservedFiles` in
the test harness independently checks old-file paths, lengths and full-content CRC32
before and after add-files. Header/checksum success is not a guarantee of recovery
after sudden power loss.

PC tests in `test/sessionChecks.cpp` use the real backend/writer with a simulated
filesystem in RAM and deliberately introduced failures. They are included in the
repository smoke runner. The additional `writeBatchChecks.cpp` uses the firmware's
128-KiB pump size to check write sizes/alignment across incoming batches and file
changes. These checks do not measure real SD performance. Real-card evidence comes
from the storage tests and ADC runs described above; PC checks alone do not establish
it.

 `RetainedPayloadChecksums` is an optional source-side verifier used by `SdRecorder`.
Before acquisition it receives the prepared file capacities. During acquisition it
updates a checksum for each file from the bytes accepted into RAM, resetting that
checksum when a file is reused. It allocates no memory or disk space during
submission. After acquisition stops, readback checks the latest data left in each
selected file, in recording order. This detects changes between submission and
readback, but cannot verify data already overwritten. It has no ADC or board
dependency. Its PC checks cover reuse, unequal capacities, partial files, changed
bytes and missing offsets.