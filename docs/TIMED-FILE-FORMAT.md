# Timed recording: implementation and verification

Updated November 10, 2026.

The [client design](LXDIGITIZER-SAMPLE-TIMING-DESIGN.md) is authoritative for
the design, binary-layout tables, timing calculations and accuracy limits.
This companion explains the implementation, storage behavior and test procedure.
It deliberately does not maintain a second set of byte-offset tables.
For other recording modes, see [recording formats](RECORDING-FILE-FORMAT.md).

## Format and implementation names

```text
[256-byte LXDG header]
[48-byte tBlock][3 * sCount sample bytes]
[48-byte tBlock][3 * sCount sample bytes]
...
[48-byte zero end marker]
```

Current files use four-byte ASCII `LXDG`, version 1. Earlier bench files used
two-byte `DG` and different header offsets. The current Python reader rejects
those earlier files. All multibyte integers are little-endian; fields are
encoded explicitly, without C++ struct padding.

| Client design name | C++ member | Python reader field |
| --- | --- | --- |
| `tcPpsCurrent` | `_tcPpsFirst` | `tcPpsFirst` |
| `tcPpsNext` | `_tcPpsLast` | `tcPpsLast` |
| `blockPairsPerFile` | `_pairsPerFile` | `pairsPerFile` |

These are names for the same stored fields, not different layouts.
The encoder is [TimedFileFormat.cpp](../espP4/lib/timedRecording/src/TimedFileFormat.cpp).
The reader is [timedReader.py](../espP4/test/timedReader.py).

A sample is a signed 24-bit integer, least-significant byte first. `FF FF FF`
is -1, `00 00 80` is -8,388,608, and `FF FF 7F` is +8,388,607. The ADC status
byte is excluded. Zero-valued samples are ordinary data: a reader advances by
exactly `3 * sCount` bytes and recognizes the zero marker only at a tBlock position.

## Forming a complete interval

Core 0 maintains the association between FSYNC pulse numbers and hardware timer
counts. Roughly millisecond observations keep that reference current; they do
not produce tBlocks. At each PPS boundary, two consecutive captured FSYNC edges
identify the preceding block's last sample and the next block's first sample.
The boundary is shortly after PPS, not necessarily the nearest sample to it.

`FsyncBoundaryCapture` runs its short search in internal instruction RAM. It
allows up to 32 further register reads after the initial read and rejects a
search taking 20 microseconds or more. A bounded critical section prevents local
interrupts during the search. Peripheral delays can still cause skipped edges;
the code resumes from the newer actual capture rather than inventing an edge.
`FsyncBoundaryMath` requires 156 or 157 ticks between adjacent captures and uses
the recent numbered reference to identify their samples. The numbering and
PPS-to-boundary checks retain their 10-ms limits.

Core 1 decodes and packs samples into `TimedIntervalBuffer`, which has 2,097,152
three-byte slots (6 MiB). Core 0 supplies boundary records. The formatter waits
until both boundaries and all intervening samples are available. Their sample
numbers give `sCount`; their actual captured ticks supply the FSYNC endpoints.
The opening and closing PPS captures supply the second references.

Only complete intervals are retained. Samples before the opening boundary and
after the last complete interval are omitted at normal startup/shutdown.
Consecutive retained blocks neither duplicate nor omit samples between them.
Missing PPS, invalid timing, sample loss or buffer exhaustion faults and stops
acquisition; the code does not silently substitute nominal timestamps.

## Formatting, ownership and file rotation

`TimedRecorder` connects interval staging to `TimedRecordingStream`,
`TimedBufferedSink` and `TimedFatStorage`. The formatter and disk writer are
separate Core 0 tasks, so SD waiting does not prevent formatting. In the tested
configuration, timing/control has priority 5, the disk writer 4, formatter 3,
and the Core 1 decoder 10. Regular task sleeps allow other work to run.

The formatter calls `beginBlock` after metadata is final, then `submitSamples`
until exactly `sCount` samples have been accepted. A submission does not cross
an sBlock boundary. The sink copies the submitted bytes into its 16-MiB ring;
acceptance means the bytes are retained in RAM, not yet written to SD. These
calls do not perform disk I/O or lend mutable buffer pointers to the caller.

A separate queue holds up to 32 file-boundary descriptors. Each describes a
file's starting byte position, slot and eventual closing position in the byte
stream. Atomic flags make completed descriptors visible to the disk task;
that task releases a descriptor only after the file closes. Either queue or
ring exhaustion is a reported fault.

`TimedRecordingConfig` defaults to 60 pairs per file, ten file slots, circular
overwrite and a 2,553-ns correction. Configuration is supplied while idle;
runtime terminal/web controls remain separate work. At rotation, `nSec` resets
to zero and the new header's UTC origin advances by the completed interval count.
The header's pair limit is not the number actually written. A normal final file
can contain fewer complete pairs.

The formatter submits the zero end marker and queues file closure when a file
is finished. Only a subsequent interval opens the next file. With overwrite
disabled, exhausting the file pool reports full. With overwrite enabled, reuse
is confined to this session's slots. The header is not rewritten on completion.

`TimedFatStorage::prepare` creates a fresh `TIMED/Snnnnnn` directory and
preallocates `F0000.DG`, `F0001.DG`, and so on. It does not format the card or
delete existing sessions. Failed allocation can leave unused new files. Each
slot has a checked capacity with drift headroom; unused trailing space remains
allocated. A reader stops at the zero marker and ignores that tail.

## SD transfer alignment and throughput

The write ring supplies 128-KiB chunks. Those chunks are independent of tBlocks,
sBlocks and file boundaries. `TimedBufferedSink::write` splits a chunk when it
crosses a file boundary, closing the old file before opening the next.

`TimedFatStorage` accumulates bytes in a 16-KiB internal, DMA-capable transfer
buffer. The P4 allocator provides 64-byte memory alignment. Full writes start at
16-KiB offsets relative to each file's byte zero, even when incoming ring spans
have awkward lengths. Closing writes the exact short tail without padding;
the next file starts with an empty transfer buffer.

This distinction matters: a file can end at an arbitrary byte offset. Aligning
transfers to global ring chunks allowed subsequent files' writes to become
misaligned. Accumulating transfers relative to each file removed the sustained
slowdown observed after rotation. The five-minute test passed with the existing
16-MiB ring and unchanged timing limits.

A copy into the transfer buffer is not yet a completed SD write. The adapter
owns those pending bytes after the ring span is released. Statistics distinguish
ring-queued bytes from transfer-pending bytes; final unsaved counts include both.
A write failure is latched rather than silently retried. Completion requires the
pending tail, synchronization and close operations to succeed.

Other essential changes were keeping the decoder's common path inline, placing
the time-critical decoder and boundary search in internal instruction RAM, and
separating formatting from disk writing. After acquisition, long diagnostic
reports yield between rows, and readback yields every 256 KiB. These pauses allow
other tasks to run without disabling the watchdog or changing captured times.

## Latest hardware verification: five minutes passed

Capture `20261110-044459`, run `1620634281`, saved files under
`0:/TIMED/S000012`. This run used the current `LXDG` header.

| Measurement | Result |
| --- | --- |
| Usable FSYNC pulse-number comparisons | 192,011 matched; zero disagreements |
| Longest observation gap | 1,163.062 microseconds |
| PPS captures and boundaries | 300; no missing or failed boundaries |
| Timer wraps | 11 |
| Complete tBlock/sBlock pairs | 299 |
| Files and rotation | Five files; four rotations; 60/60/60/60/59 pairs |
| Retained samples | 306,174,907 |
| Verified bytes and CRC32 | 918,540,593; `cd0b6da2` |
| Peak sample staging | 1,027,624 sample slots |
| Peak write ring | 4,383,631 of 16,777,216 bytes |
| Maximum measured write operation | 297,881 microseconds |
| Reported receive drops, storage errors and unsaved bytes | Zero |

Startup, ongoing numbering and stopped-counter checks passed. All five files
passed full SD CRC readback. The byte count covers headers, tBlocks, sample
payload and end markers, excluding unused preallocated tails. The log is
[the saved P4 report](../captures/fsync-association/20261110-044459/p4.txt);
local capture folders may not be available in a fresh checkout.

The generator supplied PPS and the header used synthetic UTC `946684800`
(2000-01-01). This verifies the tested timing/recording path, not real GPS UTC
association or an absolute timing error bound. Five fresh files were preserved;
live circular reuse of this timed format was not exercised. The earlier
one-minute trials are historical evidence, not the latest qualification result.

The normal format stores no payload CRC or GPS-validity flag. The bench compares
a CRC accumulated during serialization with a full readback CRC. A later reader
cannot establish payload integrity or GPS validity from the header alone. The
zero marker denotes normal completion, not guaranteed power-loss recovery.

## Running and checking the test

After building/uploading the FSYNC diagnostic and matching Feather bench
firmware, reset both boards. Use the verified generator PPS on GPIO6 and its
divided branch on CH0P; FSYNC remains on GPIO1. Power the ADC and both boards.
From `espP4`, with the ports used in the passing run:

```powershell
python .\test\runFsyncAssociation.py --timed-sd-test --runs 1 --p4 COM3 --feather COM4
```

This selection requests 307,200,000 samples (about five minutes), allocates five
fresh files, and preserves previous sessions. Allow at least 1 GB free space.
The Feather safety timeout is 600 seconds; the host allows up to 20 minutes for
preparation, capture, shutdown, reporting and readback. Wait for the final
`Timed PASS` before removing the card. Ordinary RAM PPS and sample-only SD test
selections remain shorter tests; they are not this timed-format rotation test.

The host checks retained counts and file lengths derived from PPS boundary
records, requires five files and 298–300 complete intervals, and checks the
reported full readback result. The successful run retained 299 intervals.

Desktop checks, also run from `espP4`:

```powershell
.\test\runDecoderChecks.ps1
python .\test\timedReaderChecks.py
```

These exercise encoding, staging, signed samples, exact time calculations,
rotation/reuse, buffering, old-file preservation and injected storage failures.
They do not operate the boards or establish hardware throughput.

For PC readback, `TimedReader` validates the layout and retains metadata while
memory-mapping sample bytes. `sampleTime` returns integer UTC seconds plus an
exact fractional second using Python `Fraction`. Convert to nanoseconds as in
the client design; do not first convert the large epoch timestamp to a float.
Exact arithmetic prevents numerical precision loss, not physical timing error.
