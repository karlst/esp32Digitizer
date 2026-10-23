# Session file recorder

`FileRecordingBackend` owns the byte ring and a pool of preallocated FatFs files.
It has no ADC, Feather, command parser or reporter dependency. The P4-only choke
tests and real ADC recording now use it. `SdRecorder` is the asynchronous ADC
adapter: it packs samples, schedules backend calls on its disk task and checks
a combined readback checksum. The ADC integration still needs build/hardware
verification; the three storage-only modes passed on hardware.

## Session parameters

A session is one run, beginning with `prepare` and ending with `finish`.
`RecordingSessionConfig` supplies:

- `_secondsPerFile`: payload capacity = seconds × sample rate × bytes per sample.
  The 512-byte header is additional. Rollover follows bytes, not a timer.
- `_fileCount`: number of files to allocate, **additional** files for addFiles.
- `_preparation`: `formatAndAllocate`, `addFiles`, or `useExisting`.
- `_fullPolicy`: `overwriteOldest` or `stopWhenFull`.
- `_sampleRate`, `_bytesPerSample`, `_sampleFormat`: caller-supplied recording
  metadata. The backend does not configure or interpret an ADC.

Fresh formatting is explicit. Failed mounting never triggers formatting.
Fresh/add select only their newly allocated files for the session's pool.
Add-files preserves all previous recordings and unrelated files. Allocation
failure leaves any new/partial files for inspection; it does not delete old ones.

Use-existing ignores count/duration, scans compatible prepared headers, and
selects unused files first. With overwrite enabled it also selects older
recordings, ordered by session and generation. With stop-when-full it selects
only unused slots and refuses preparation if none remain. File capacity stays
unchanged, including when the final recording uses only part of a file.

The managed directory is `RECORD`, with names `R0000000.BIN` onward. Session IDs
increase from the largest recognized header; formatting starts a new namespace
at 1. There is no catalog. Unrecognized or damaged headers are never candidates
for reuse, and their names remain occupied. Scanning rejects directories above
4096 entries rather than silently ignoring files.

`preallocate` supports a separate setup step: use fresh/add preparation to reserve
unused files without opening a recording or starting the ring. It shares the
same allocation implementation as `prepare`. Call `prepare(useExisting)` later
to record into that prepared capacity. Never call either while acquisition is active.

## Calls and ownership

Build all users with `BUFFERED_WRITER_EXTERNAL=1` and identical ring capacity/
block-size definitions. `initializeMemory` allocates the ring through caller
callbacks and internal scratch/staging before acquisition. The caller also
supplies monotonic microseconds and valid UTC seconds. The P4 adapter selects
the SD pins, card voltage, clock and explicit exFAT format.

One disk task calls `prepare`, `pump`, `finish`, `verify` and `getStatistics`.
One producer task calls `submit` and optionally `skipInput`. Stop the producer
before `finish` or another `prepare`. Objects must outlive both tasks. On the
P4, the ring is PSRAM; do not submit from a cache-disabled interrupt.

`prepare` is synchronous: true means files and ring are ready, not merely that
preparation was queued. `submit` copies without allocating, accessing disk or
waiting for a write. Batches must contain whole samples and at most 65,520 bytes.
It returns the accepted payload count. Ordinary overflow drops the whole batch
and continues; under stop-when-full a final prefix may be accepted. The caller
checks `getFailed` and `getFull` between submissions and accounts for rejections.

The ring contains only payload. Incoming batch boundaries do not break up the
128-KiB disk batches; those are copied through 16-KiB internal scratch as before.
Only reaching file capacity triggers a file change. For sector-multiple file
capacities, regular writes retain 512-byte sector alignment. Non-sector-multiple
capacities and the final short drain are supported but can require partial writes.

Overflow drops the incoming batch, counts the missing bytes, and keeps using the
current file when space returns. `skipInput` adds known missed input, such as
missed generator deadlines. Neither operation closes a file or writes a header.
After acquisition stops and accepted data drains, `finish` saves the final session
loss total in every retained header. The total is repeated, not apportioned per
file; do not sum it across files. Exact gap locations are not recorded. A session
with loss must not be interpreted as a continuous, evenly spaced time series.
An unfinalized loss flag means the total is unknown, not zero.

`pump` retains ring memory until the write finishes. Short writes, I/O errors and
flush failures stop the backend; a partial prefix is never replayed. `finish`
drains and closes, marking only successfully completed payload as complete.
The caller must still examine losses before claiming a loss-free session.

Statistics are disk-task-owned; publish a synchronized copy for a third reporting
task. `_written` counts only payload, including generations later overwritten.
The ring and its `_buffer` counters also contain only payload. `_lostBytes` is
published by `finish` after the producer has stopped; it is not a live loss
snapshot. The test continues to publish its own live loss counter. Peak occupancy
and maximum buffered write/flush latency include file changes during capture;
the later loss-header finalization pass is outside that measurement.

## Header and verification

[Header layout and hardware procedure](../../../docs/SD-CHOKE-TEST.md#file-header)
describe the 512-byte format, UTC/text, session/generation, capacity, valid length,
session loss total, finalization flag, state and CRC32. UTC is file opening time, not precision sample time.
Unused payload tails are not valid data. An incomplete file's last saved header
does not claim all payload that may have reached disk before failure.

`verify` compares retained files against this session's in-memory headers and
optionally calls a read-only payload checker in saved-stream offset order.
The context overload lets a caller accumulate a checksum or waveform across
files without globals. Callbacks borrow scratch only until return; false rejects
verification. `pump(true)` permits short blocks during caller-paced final drain. It does not verify overwritten
generations or old files. `PreservedFiles` in the test harness independently
checks old-file paths, lengths and full-content CRC32 before and after add-files.
Header/checksum success is not a guarantee of recovery after sudden power loss.

PC tests in `test/sessionChecks.cpp` use the real backend/writer with RAM FatFs
and injected failures. They are included in the repository smoke runner. The additional `writeBatchChecks.cpp` uses the firmware's 128-KiB pump size to
check write sizes/alignment across incoming batches and file changes. These
checks do not measure real SD performance. The corrected backend passed the three storage-only hardware tests; new
ADC readback-context checks and integration still need Karl's verification.


`RetainedPayloadChecksums` is an optional source-side verifier used by `SdRecorder`.
Before acquisition it receives the prepared file capacities. During acquisition
it computes one incremental checksum per surviving generation from accepted
source bytes, without allocating or touching disk. After acquisition stops,
readback checks each surviving generation in stream order. This catches changes
between submission and readback, but cannot verify generations already overwritten.
It has no ADC or board dependency. Its PC checks cover reuse, unequal capacities,
partial files, changed bytes and missing offsets.
