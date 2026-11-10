# Recording formats: choose the correct reader

Two recording paths remain in the source. Their files are not interchangeable.
The [client design](LXDIGITIZER-SAMPLE-TIMING-DESIGN.md) describes the timed
path; it does not redefine files produced by the sample-only recorder.

| Path | File structure | Timing information |
| --- | --- | --- |
| Timed recording (`--timed-sd-test`) | 256-byte `LXDG` header; repeated 48-byte tBlock and three-byte samples; 48-byte zero end marker | Captured FSYNC and PPS counts support per-sample time calculation |
| Sample-only session recorder | 512-byte session header followed by sample payload | File-open UTC and nominal sample rate; no per-interval FSYNC/PPS records |

## Timed recording

Use the [timed-format implementation guide](TIMED-FILE-FORMAT.md) and
[`timedReader.py`](../espP4/test/timedReader.py). The client design contains the
authoritative byte-layout tables and sample-time calculation. Current files use
four-byte `LXDG`, version 1. Earlier bench files with two-byte `DG` have different
header offsets and are rejected by the current reader.

## Sample-only session recording

The file-pool backend uses
[`SessionFileHeader`](../espP4/lib/fileRecorder/src/SessionFileHeader.h), encoded by
[`SessionFileHeader.cpp`](../espP4/lib/fileRecorder/src/SessionFileHeader.cpp).
Its identifier is `RINGSD02` at byte offset 64. The header records session and
file sequence, capacity, valid payload length, stream offset, file-open UTC,
sample rate, sample width/format, completion state and loss totals.

Read only the declared valid payload bytes, excluding the 512-byte header and
unused preallocated tail. ADC payload samples are signed 24-bit integers in
three little-endian bytes. Storage tests may use a different sample width and
format; inspect the header rather than assuming every session file is ADC data.
The header checksum covers the header, not the sample payload. The backend
updates completion information; the timed format instead uses an end marker.

File-open UTC is not a captured PPS reference and cannot substitute for the
timed format's per-sample association. See the
[file-recorder library](../espP4/lib/fileRecorder/README.md) for backend behavior.

## Older compatibility formats

[`recordingFormat.h`](../espP4/main/include/recordingFormat.h) still contains the
sample-packing helper and older `P4ADC03` header helper. That header is retained
for compatibility/tests; it is not the current file-pool session header or the
`LXDG` header. Select a reader by the actual identifier and version, not just the
filename or a shared three-byte sample width.
