# Recording file format, version 1

Both exFAT and FAT16/FAT32 are supported without reformatting.

Files are `/recordings/recording_NNNNNN_PPP.bin`. The first number identifies a
session; the second identifies a part. Start chooses a number above existing
recordings. Each part is created exclusively; existing files are never overwritten.
Parts split before 1 GiB. Deleting all recordings allows numbering to restart.

## Header

Every part begins with 512 bytes. All numbers use little-endian byte order.
Reserved bytes are zero. Readers must reject unknown header or sample-format
versions rather than assume they contain today's sample encoding.

| Byte offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 8 | ASCII `S3REC001` |
| 8 | 4 | Header size: 512 |
| 12 | 4 | Header version: 1 |
| 16 | 4 | Sample format: 1 = signed 32-bit little endian |
| 20 | 4 | Requested samples/second |
| 24 | 4 | Bytes per sample: 4 |
| 28 | 4 | Disposition: 0 open, 1 normally closed, 2 incomplete |
| 32 | 8 | Successfully written payload bytes in this part, updated on close |
| 40 | 8 | Index of this part's first sample in the session, starting at zero |
| 48 | 4 | Session number |
| 52 | 4 | Part number |
| 56 | 456 | Reserved |

The initial header is synchronized before acquisition begins. The closing header
is written after pending samples have been written and synchronization attempted.
An ADC fault or recording failure marks the final part incomplete. A previous part
can be normally closed even if a later part fails; inspect the entire session.
A card failure can prevent rewriting the header, leaving disposition 0. Sudden
power loss can also leave filesystem damage. Disposition 1 reports successful
software completion; it does not establish that no physical ADC events were missed.

## Payload

Each accepted ADC value is sign-extended from 24 bits into a signed 32-bit word.
Examples: -1 is `FF FF FF FF`; -8388608 is `00 00 80 FF`; 8388607 is
`FF FF 7F 00`. Values are raw counts, not volts. There are no per-sample timestamps
or GPS bits in version 1. Changes to packing/GPS require a new sample-format ID.

For a normally closed part, file length must equal 512 plus the header's payload
byte count, which must be divisible by four. An incomplete file can end inside a
sample after a partial write; discard that trailing fragment. Never treat an
open header's zero payload count as proof that the file contains no samples.

The UI's bytes-written counter measures successful write calls, including header
rewrites, so it differs from final file size. Samples-written counts only complete
payload records accepted by the filesystem. Flush failures leave durability unknown.

## Temporary Choke test files

The `choke-test` firmware uses the same numbered filenames and four-byte signed
sample encoding, but its magic is **`S3CHK001`**. These are generated test values,
not ADC measurements. The sample-rate field at byte 20 is zero because the rate
changes. Three fields replace the first twelve reserved bytes:

| Byte offset | Bytes | Meaning |
| --- | --- | --- |
| 56 | 4 | Initial stored-bit rate: 750000 bits/s |
| 60 | 4 | Rate increment: 250000 bits/s |
| 64 | 4 | Time per rate: 10000 milliseconds |

Remaining bytes 68–511 are zero. The ramp counts all 32 stored bits per word.
Payload starts at zero and increments through the signed 24-bit range, wrapping
every 16,777,216 words. A new Start restarts the counter and rate ramp; part rollover
does not. Timing fields specify the intended schedule, not measured arrival times.
User Stop produces disposition 1 if saving succeeds; automatic choke/producer-lag
stop produces disposition 2 even when all previously accepted words can be saved.
The inspection script recognizes both magics and labels the source explicitly.
