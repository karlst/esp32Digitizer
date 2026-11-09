# November 7, 2026 — sample timing and timed SD recording

This is the chronological work record. The companion
[sample timing design](SAMPLE-TIMING-DESIGN.md) explains the final alignment,
file/readback algorithms and accuracy evidence together.

## Result at the stopping point

The 60-second **new-format recording test passed**. The P4 associated simulated
PPS pulses with numbered ADC samples, formed complete tBlock/sBlock pairs,
wrote them to SD while acquisition continued, and verified every serialized
byte by reading it back. This is the first passing live test of the new format.

The final capture is `captures/fsync-association/20261107-153348`, run
`984129830`. The SD file is `0:/TIMED/S000005/F0000.DG`.
Capture directories are local evidence; this report preserves the essential
results even if those directories are not included in a source commit.

| Final measurement | Result |
| --- | ---: |
| PPS pulses captured and mapped | 60 |
| Adjacent sample boundaries captured | 60; zero failed or missing |
| Complete tBlock/sBlock pairs | 59 |
| Retained samples | 60,415,138 |
| Serialized bytes, including metadata and end marker | 181,248,550 |
| Full SD readback CRC32 | `0a79e9d4`, matching submitted bytes |
| Write errors / unsaved bytes | 0 / 0 |
| Staging peak | 1,026,927 of 2,097,152 sample slots |
| Write-ring peak | 1,818,316 of 16,777,216 bytes |
| Maximum reported write/flush duration | 110,850 microseconds |
| 32-bit capture-timer wraps handled | 2 |
| Independent numbering comparisons | 25,243 matched; zero mismatches |
| Stopped endpoint sample-number error | 0 |
| Longest interval between numbering observations | 9,038.881 microseconds |

The host concluded: `Timed PASS: complete tBlock/sBlock counts and full SD CRC
readback succeeded; UTC is synthetic.` Sixty boundaries provide 59 intervals
between them. Samples before the first boundary and after the last complete
interval are deliberately omitted.

## Bench configuration

The ADS127L18 supplies nominally 1,024,000 samples/second. Each retained sample
is three bytes, giving 3,072,000 sample bytes/second before metadata. The serial
interface uses 32 DCLK periods per sample at 32.768 MHz. A sample period is
976.5625 ns. A sample means one ADC reading of CH0, not one serial bit.

The P4 runs at 360 MHz and has 32 MiB of external PSRAM. Its nominal 160-MHz
capture timer advances once per 6.25 ns. There are nominally 156.25 capture
ticks per ADC sample; timer resolution is not a claim of absolute accuracy.

| Connection | Purpose |
| --- | --- |
| DCLK to GPIO0 | Clock for receiving serial data |
| Translated FSYNC to GPIO1 | Identify ADC frame boundaries and capture their times |
| DOUT0 to GPIO2 | CH0 sample bits |
| Rigol output to GPIO6 | Simulated PPS rising edge |
| Same output through the verified divider to CH0P | Analog test signal |
| CH0N and circuit grounds | Established common reference |
| P4 COM3 / Feather COM4 | PC control connections |

The generator was verified at 1 Hz, 10% duty, approximately 0–3.3 V
(3.3-V amplitude setting and +1.65-V offset). CH0 uses the divided voltage
and the existing 2.5-V ADC reference. No GPS was needed for these tests.

The accepted 2553-ns input-to-FSYNC correction comes from the earlier delay
measurements, including the voltage translator in that measured path. It was
not remeasured today. This work does not establish its complete absolute
uncertainty or validate UTC from a real GPS receiver.

## Establishing which FSYNC belongs to which sample

FSYNC feeds two hardware peripherals: PCNT counts rising edges, and MCPWM
capture saves the timer count at an edge. A rising edge is the signal's
transition from low to high. These are separate registers; reading one and
then the other is not an atomic snapshot of both.

The initial diagnostic included extra GPIO reads and ordering instructions
called fences. Its fastest complete comparison took 1055.556 ns, longer than
the 976.5625-ns sample period, so all 10,000 sets exhausted their 32 attempts.
That measured the instrumented procedure, not just register access.

Isolated measurements then showed two pairs of register reads averaging
502.468 ns; the explicitly ordered version averaged 547.381 ns. A later lean
comparison averaged 494.401 ns. Of 16,864 attempts, 6,886 were inconsistent
(40.83%). This percentage includes retries: it is **not** the percentage of
first attempts that failed. Twenty-one of 9,999 requests exhausted all 32
attempts (0.21%). Repeated equal values alone do not make the two hardware
updates atomic.

The subsequent numbering method keeps a recent reference and calculates the
number of intervening FSYNC periods from captured tick differences. It rounds
to a whole number of samples, checks that the result is comfortably away from
a rounding boundary, and compares with the independent pulse counter whenever
a usable comparison is available. It does not silently repair a disagreement.
Observations are frequent, roughly once per millisecond, even though PPS is
only once per second. These observations maintain the numbering reference.

The maximum allowed gap is 10 ms; the allowed residual is less than a quarter
sample. A gap or ambiguity beyond the guard stops the test. The final run's
9.039-ms maximum gap passed, but leaves limited observed margin; it is not a
worst-case scheduling guarantee.

Startup has an explicit convention: discard the initial incomplete frame
(31 clocks in these runs). Stored sample zero belongs to FSYNC pulse 2.
The stopped endpoint compares the final predicted pulse number with the
settled hardware count. This passed with zero offset in the final run.
The final incomplete DMA delivery is reported separately from retained samples.

Milestones: `111915` verified relative numbering; `112733` verified the
sample-zero convention; `125428` mapped ten simulated PPS pulses; `130039`
mapped sixty and handled two timer wraps. These suffixes refer to capture
folders beginning `20261107-` under `captures/fsync-association`.

## Connecting PPS to sample timing

MCPWM captures GPIO6 using the same timer as FSYNC. Its interrupt service
routine (ISR, code called when hardware signals an event) copies the captured
value. The saved time is the hardware edge time, not the time software gets
around to handling the interrupt. Software extends the 32-bit values across
wraparound into 64-bit counts.

For each PPS, the boundary routine obtains two actual consecutive FSYNC
captures shortly afterward. The earlier sample ends the preceding sBlock;
the later sample starts the following sBlock. Their spacing must be 156 or
157 ticks. The boundaries can occur after PPS rather than at the very first
sample following PPS. The file retains both the real PPS time and the real
sample endpoint times, so this delay does not become a sample timestamp error.

If a read misses an edge, the routine moves its candidate forward and looks
again. It allows at most 32 reads and rejects a search reaching 20 microseconds.
A short critical section prevents ordinary local interrupt interference;
the other core and DMA continue. There is no interrupt on every FSYNC.
Numbering still has to pass its separate checks.

Early attempts skipped several FSYNC periods: `144504` observed a 625-tick
gap, and `144954` observed 1250 ticks with a search lasting over 23 microseconds.
The capture routine was in flash. Moving it into internal instruction RAM
preceded the passing `145319` run: 60 adjacent boundaries with no failures.
The code placement was checked in the compiled image. The counters did not
directly measure cache misses, so cache stalls remain the explanation being
addressed, not an independently measured cause.

## Recovering throughput — essential changes to preserve

### Keep the common decoder path small and inline

The first SD runs dropped raw receive buffers. Comparison with the earlier
compiled decoder showed that startup additions had stopped the compiler from
inlining `FrameDecoder::beginFrame`. That introduced a function call for every
sample. The fix keeps the normal frame-start path inline and moves exceptional
checks to a separate non-inline function. Startup checks stop running once
startup is resolved; integrity checks remain.

The sample-only combined test then passed in `135357`: 184,338,333 bytes,
CRC `4a00c092`, all 60 PPS mapped, and no reported capture/storage losses.
This established a working comparison before adding the new format.

### Let timing/control work run regularly

The first timed-format run (`150920`) wrote an interval but exceeded the
10-ms numbering observation guard: 10.234 ms. Its priority-4 worker slept
only every sixteen iterations. The worker was changed to sleep one scheduler
tick after every iteration, matching the earlier writer. A scheduler tick
here is 1 ms. This yields CPU time to other tasks; it does not set sample times.
The timing guard was not relaxed.

### Match the working SD scratch allocation

Run `151239` kept timing observations within the guard but filled staging.
The new SD adapter had omitted the earlier writer's alignment and DMA memory
flags. Its 16-KiB transfer scratch now uses 64-byte-aligned internal memory
with `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT`. DMA means the
controller transfers memory directly without the CPU copying each byte.
The previous allocation's actual alignment was not logged, so this correction
alone cannot be credited with a measured speed improvement.

### Separate formatting from disk waits

Run `151737` still filled staging after about nine seconds while the large
write ring was nearly empty (143,193-byte peak). One task was doing both
formatting and disk pumping; while it waited for SD, it could not move staged
samples into that ring.

Formatting and disk pumping now run as separate Core 0 tasks, priorities 3
and 4 respectively. A task is a scheduled software thread. Formatting can
continue while the disk task waits for hardware. Both sleep one tick after
each work cycle. No buffer capacity was increased to obtain the pass.

### Put the frequently executed decoder code in internal instruction RAM

The split-task run (`152907`) used the write ring without filling staging,
but dropped two raw DMA deliveries when all sixteen receive buffers were
occupied. Maximum queue age was 7.425 ms. `FrameDecoder::feed` and
`FrameDecoder::finishWord` were still executing from flash.

Both functions now reside in internal instruction RAM (IRAM), as does the
short boundary-capture routine. The compiled image confirmed `.iram0.text`
placement. Desktop builds retain the same algorithm without ESP-specific
placement attributes. The final run passed; maximum queue age fell to
2.073 ms. Neither decoding rules nor receive-queue capacity changed.

**Internal RAM does not mean all buffers moved there.** The large 6-MiB
staging buffer and 16-MiB write ring remain in external PSRAM. Internal RAM
holds the critical instruction paths and aligned SD transfer scratch.

## Final transfer path and memory ownership

1. Hardware DMA receives raw serial data. Sixteen owned receive buffers queue
   deliveries for the decoder; a full queue is a fault, not silent overwrite.
2. `SampleDecoder` on Core 1, priority 10, calls `FrameDecoder`. Decoded batches
   contain up to 512 words; a delivery can end before that limit.
3. `FsyncTimedTest::submit` forwards the batch to timing diagnostics and
   `TimedRecorder::submit`, which copies packed three-byte samples into
   `TimedIntervalBuffer`. The decoder does not perform SD writes.
4. Control code on Core 0 submits boundary records into an eight-entry queue.
   Completed records are made visible with atomic indices; their 64-bit fields
   are not read while another task is still constructing the record.
5. The formatter waits for both endpoints and all samples of a closed interval.
   It submits metadata and copies at most 4096 samples per work cycle through
   `TimedRecordingStream` into `TimedBufferedSink`'s write ring. Staging slots
   become reusable only after this copy succeeds.
6. The disk task drains fixed 128-KiB chunks, using internal transfer scratch
   for actual FatFs writes. A chunk can split a sample block or metadata record;
   it is not the same thing as an sBlock.

Staging has 2,097,152 sample slots (6 MiB). Roughly one second of storage is
needed simply to wait for the closing boundary. Atomic head/tail positions
prevent simultaneous writers from reusing unread memory. The reusable timed
storage components do not depend on the ADC driver or Feather protocol.

On shutdown, acquisition and boundary submissions stop first. The formatter
drains complete intervals, omits the incomplete tail, and queues the zero end
marker only for a clean finish. It then releases a completion flag. The disk
task waits for that flag before draining, closing, and reading back the files.
Accepting a submission, writing it, and verifying it are distinct states.
Overflow, invalid timing, or storage failure stops recording and reports a fault.

## File format actually written

See [the format specification](TIMED-FILE-FORMAT.md) for the maintained layout.
Integers are little-endian and encoded explicitly, without C++ structure padding.

| Header offset | Field | Size |
| ---: | --- | ---: |
| 0 | ASCII `DG` | 2 bytes |
| 2 | Version, initially 1 | 2 |
| 4 | `utcFirst`, UTC seconds for the first retained PPS | 8 |
| 12 | `delayNs`, signed correction, currently 2553 | 4 |
| 16 | `pairsPerFile`, currently 60 | 4 |
| 20 | Reserved zeros | 236 |

Each 48-byte tBlock contains:

| Offset | Field | Size |
| ---: | --- | ---: |
| 0 | `tcFsFirst` | 8 bytes |
| 8 | `tcFsLast` | 8 |
| 16 | `tcPpsFirst` | 8 |
| 24 | `tcPpsLast` | 8 |
| 32 | `sCount` | 4 |
| 36 | `nSec` | 2 |
| 38 | Reserved zeros | 10 |

Exactly `sCount` signed 24-bit samples follow each tBlock. There is no sample
number, validity flag, or per-block CRC in the file. A 48-byte all-zero tBlock
marks normal completion. The header is not rewritten at shutdown. The reader
ignores unused preallocated space after the marker.

The final file's length is independently explained by:

`256 + 59 × 48 + 60,415,138 × 3 + 48 = 181,248,550 bytes`.

Runtime defaults are 60 pairs/file, ten files, circular overwrite. This live
test uses one freshly allocated slot with overwrite disabled; older sessions
are preserved. Multi-file rotation and reuse have desktop checks, but were not
exercised by this new-format one-file hardware run.

For sample index `i`, starting at zero within its sBlock:

```text
dtcSample = (tcFsLast - tcFsFirst) / (sCount - 1)
tcSample  = tcFsFirst + i * dtcSample
time      = utcFirst + nSec
            + (tcSample - tcPpsFirst) / (tcPpsLast - tcPpsFirst)
            - delayNs / 1,000,000,000
```

Both PPS counts are in the same tBlock, so readback need not fetch the next
one to calculate sample times. The measured PPS interval supplies clock-rate
correction. The Python reader uses integer seconds and exact fractions rather
than rounding a large epoch timestamp to floating-point seconds. This avoids
arithmetic precision loss; it does not remove physical measurement uncertainty.

## Verification and limits

Desktop checks cover encoding, reserved bytes, signed samples, reader behavior,
staging wraparound, delayed delivery, omitted startup/shutdown tails, rotation,
overflow and storage failures. Karl ran the checks and firmware builds during
implementation. The final hardware run additionally checked boundary records,
sample counts, file length, timer wraps, and full SD readback CRC.

The final reported formatting time was 15,783,677 us and disk-pump time
53,367,119 us over 9,534 work cycles. These are elapsed task measurements:
they include preemption/waits and can overlap. They must not be added together
or treated as exclusive CPU utilization. Observed maxima are not worst-case
latency guarantees.

This test used synthetic UTC `946684800` (2000-01-01). It did not verify real
GPS message/PPS association, GPS validity or missed-pulse behavior, absolute
nanosecond accuracy, a long-duration recording, or new-format live circular
reuse. CRC readback verifies the accepted byte stream, not every possible
physical failure or persistence after power removal.

## Resume here

The next useful step is to read the actual `.DG` file from the SD card with
the existing Python reader, inspect its tBlocks and sample times, and compare
the decoded CH0 waveform with the known generator input. GPS can remain
disconnected for that step. Real GPS UTC association and the production
start/stop/configuration UI remain subsequent work.

For source review, start with `TimedRecorder`, then `TimedIntervalBuffer`,
`TimedRecordingStream`, `TimedBufferedSink`, and `TimedFatStorage`. For timing,
read `FsyncIndexTest`, `FsyncBoundaryCapture`, `FsyncBoundaryMath`, and
`PpsSampleTest`. `FsyncTimedTest` connects these services for the bench test;
`runFsyncAssociation.py` checks its reports. Git review and commit remain Karl's.
