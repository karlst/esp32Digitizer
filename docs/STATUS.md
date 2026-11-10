# Project status

Updated November 10, 2026.

The [client design document](LXDIGITIZER-SAMPLE-TIMING-DESIGN.md) is the
authoritative description of sample timing. This page records current validation
and remaining work; it is not a session-by-session history.

## Latest result: five-minute timed recording passed

Run `1620634281`, capture `20261110-044459`, completed acquisition, file rotation,
and full SD CRC readback using simulated PPS and synthetic UTC.

| Check | Result |
| --- | --- |
| Usable FSYNC pulse-number comparisons | 192,011 matched; zero disagreements |
| Longest numbering-observation gap | 1.163 ms, below the 10-ms limit |
| PPS pulses and sample-block boundaries | All 300 accounted for; no failed or missing boundaries |
| Hardware timer wraps | 11 |
| Retained complete intervals | 299 |
| Files | Five: 60, 60, 60, 60 and 59 tBlock/sBlock pairs |
| Saved samples | 306,174,907 |
| Verified serialized bytes | 918,540,593; CRC32 `cd0b6da2` |
| Peak write-ring use | 4,383,631 bytes (4.18 MiB) of 16 MiB |
| Longest measured write operation | 297,881 microseconds (298 ms) |
| Reported receive drops, storage errors and unsaved bytes | Zero |

Startup and stopped-counter checks passed. Startup/shutdown partial intervals
were omitted as designed. Files are in `0:/TIMED/S000012` on the card.
The saved PC log is
[`captures/fsync-association/20261110-044459/p4.txt`](../captures/fsync-association/20261110-044459/p4.txt).
Capture folders are local evidence and may not be present in a fresh checkout.

This verifies the tested acquisition/timing/storage path, including four file
rotations. It does not establish absolute GPS accuracy, power-loss recovery,
or live circular overwrite of the new timed format. The test preserved all five
files; the configurable defaults remain 60 pairs per file and ten circular slots.

## Changes essential to keeping up

- Keep the decoder's common path inline, and the time-critical decoder and
  adjacent-FSYNC capture routines in internal instruction RAM.
- Keep the 16-KiB SD transfer buffer in aligned, DMA-capable internal RAM.
  DMA lets the SD hardware transfer bytes without a software copy for every bit.
- Separate interval formatting from disk writing. Core 1 decodes; Core 0 handles
  timing, formatting and storage. Timing/control priority is 5, writer 4,
  formatter 3, and the Core 1 decoder 10. Regular task sleeps allow other work.
- Align full SD transfers to each file's own 16-KiB offsets, independently of
  the ring's 128-KiB chunks. Flush the short tail without padding at close.
  This removed the sustained slowdown seen after rotation.
- Allow other tasks to run during the long final report and readback, preventing
  the watchdog timeout that interrupted an earlier run after acquisition.

The successful run used the existing 6-MiB sample staging buffer and 16-MiB
write ring. Neither the buffers nor the timing acceptance limits were enlarged.

## Timing calibration and remaining work

The adopted input-to-FSYNC correction is **2,553 ns**, including the voltage
translator and correction for the measured difference between P4 input paths.
Pulse tests near 1 kHz and at 10 kHz gave similar results. See the
[pulse-delay procedure](PULSE-DELAY-TEST.md) and
[input-skew procedure](INPUT-SKEW-TEST.md).

Still to establish:

- Associate real GPS UART date/time messages with the correct captured PPS.
  The bench used a generator and UTC origin 2000-01-01, not GPS time.
- Quantify total absolute timing uncertainty. A 6.25-ns timer tick is resolution,
  not a demonstrated end-to-end error bound.
- Verify live circular reuse for the timed format when that work is undertaken.
  Desktop checks cover reuse; this five-minute hardware run did not reuse slots.

## Bench configuration used for the passing run

Translated FSYNC goes to P4 GPIO1, DCLK to GPIO0, and DOUT0 to GPIO2.
The generator supplies simulated PPS to GPIO6: 1 Hz, 10% duty, 0–3.3 V.
Its divided branch supplies CH0P; CH0N is grounded. The ADC and both controller
boards require power. The recorded run used P4 COM3 and Feather COM4; ports can change.

The P4 uses the FSYNC diagnostic firmware with `--timed-sd-test`; Feather uses
external-source bench firmware. This result does not establish the state of
other firmware profiles or which image is currently connected.

## Documentation and source entry points

- [Design and sample-time calculation](LXDIGITIZER-SAMPLE-TIMING-DESIGN.md)
- [Timed-file implementation and verification](TIMED-FILE-FORMAT.md)
- [Distinguishing the recording formats](RECORDING-FILE-FORMAT.md)
- [FSYNC test instructions](FSYNC-ASSOCIATION-TEST.md)
- [Build instructions](BUILDING.md) and [PC smoke checks](../test/README.md)
- [Experiment selection](../espP4/CURRENT-SETUP.md)

The timed encoder, formatter and storage adapter are in
`espP4/lib/timedRecording/src`; `TimedRecorder` connects them to the application.
`espP4/test/timedReader.py` reads the files on a PC using exact fractional
arithmetic. Editable diagrams and their preview images are in `docs/images`.
