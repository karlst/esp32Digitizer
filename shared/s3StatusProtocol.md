# S3 acquisition and recording protocol

## Current recording extension: status 5, commands 3

Version 5 keeps the version-4 columns below and appends `fileOpenUs,freeSpaceMode`
(45 fields total). Feather accepts versions 2, 3, 4, and 5; missing v5 values are
unknown. An older Feather must be updated before using a v5 S3.

- `fileOpenUs` is the duration of initial file preparation, including filename
  selection, initial header and sync. Reset on Start; reported separately from
  recording write delays. Creating later split files remains a recording delay.
- `freeSpaceMode`: 0 unknown/older firmware, 1 measured, 2 estimated, 3 stale after
  a failed scan. A scan runs at startup, after initial opening but before samples
  begin, after Stop, and after deletion. No scan runs during active recording.
- The estimate subtracts growth in separately cluster-rounded recording file
  lengths from the measured baseline. Header rewrites consume no extra estimated
  space. Directory growth or library allocation ahead of length can differ;
  clamp at zero and never use this display estimate as a write permission.
- Recording elapsed time and maximum delay start after opening and the baseline
  scan, immediately before the Start reply permits sample production. The maximum
  includes payload writes, rollover, final flushing and closing, but not initial
  opening or stopped-state free-space scans. Both maximum and opening time reset
  for each new recording. Replayed/already-running Start commands do not reset them.

## Previous recording extension: status 4, commands 3

The recording-ready Feather accepts status 2, 3, and 4. Existing S3 firmware
continues working with acquisition controls. Only a fully validated status-4
frame advertises recording support; earlier versions show recording unavailable.
This contract is implemented by both firmware projects. Hardware recording
validation is recorded separately in README.md; support is not proof of card access.

Status 4 retains the first nineteen fields of status 3 in their existing order,
changes its version field to `4`, and appends the following 24 unsigned decimal
fields (43 fields total). `shared/recordingStatus.h` defines the same order:

```text
state,card,enabled,session,part,elapsedMs,bytesWritten,samplesWritten,cardBytes,freeBytes,bufferBytes,usedBytes,peakBytes,writePosition,readPosition,bytesPerSecond,latestDelayUs,maxDelayUs,maxDelayAtMs,maxDelayKind,overflows,lostSamples,writeErrors,deletedFiles
```

- `state`: 0 idle, 1 preparing, 2 recording, 3 saving, 4 saved, 5 incomplete/error,
  6 deleting. `card`: 0 unknown, 1 ready, 2 missing, 3 unsupported filesystem,
  4 I/O failure. `enabled` is the applied recording choice for acquisition (0/1).
- `session` and `part` identify `/recordings/recording_NNNNNN_PPP.bin`; zero
  session means no recording yet. Numeric IDs avoid transmitting arbitrary paths.
  Widths are minimum padding widths, not limits. Existing files are never replaced.
- All totals and durations are unsigned 64-bit decimal values. Feather sends
  recording JSON values as decimal strings to preserve exact numbers in browsers.
- `bytesWritten` includes headers and successful payload writes for this session;
  `samplesWritten` counts complete sample values. Neither includes queued bytes.
  Successful writes are not a guarantee of survival through sudden power loss.
- Space values are filesystem capacity/free bytes. Buffer capacity, occupancy,
  peak and positions are bytes, aligned to four bytes. Positions wrap at capacity.
  A full ring and an empty ring can have equal pointers; occupancy disambiguates.
  Data being written remains occupied until that write completes successfully.
- Delays measure elapsed S3 write/flush calls in microseconds. Maximum and its
  recording-relative millisecond timestamp reset at a new recording and include
  final flush. Kind: 0 no measurement, 1 write, 2 flush. Preserve final results
  after Stop and during acquisition without recording, with `enabled=0`.
- `overflows` counts buffer-full incidents; `lostSamples` counts known samples
  that could not be retained. Do not substitute observed ADC missed-edge counts
  or claim all physical losses are known. `writeErrors` counts failed storage
  operations. `deletedFiles` reports completed deletions during the delete action.
- Send status every 500 ms and promptly after command results. Feather accepts
  at most 1535 characters before newline and expires unfinished frames at 500 ms.
  S3 must size its TX buffer for a complete worst-case frame (at least 1536 bytes).

Commands from a recording-aware Feather to a status-4 S3:

```text
CMD,3,id,start,rate,record
CMD,3,id,stop,0,0
CMD,3,id,reboot,0,0
CMD,3,id,delete,0,0
```

`record` is exactly 0 or 1. Legacy commands remain supported with recording off.
Reject nonzero recording flags on other actions. Replay matching includes the
recording flag. A recording Start opens a new file before starting the ADC;
failure leaves acquisition stopped. A repeated Start cannot change rate or
recording mode during a run. Stop/Reboot must drain and close the recording first.

Preparing, Saving and Deleting are asynchronous operations with status progress.
While processing one, report its request ID with ackResult=0. Fresh matching busy
reports extend Feather's five-second command wait; they do not confirm success.
Report ackResult=1 only after completion, or 2 on failure. No automatic retries.
On recording errors, stop acquisition and clearly mark the file incomplete.

Deletion is stopped-only, after all file closing has completed. It removes only canonical `recording_NNNNNN_PPP.bin` regular files
from `/recordings/`; unrelated files and folders remain untouched. The browser asks
for confirmation and POSTs `confirm=delete-recordings` to `/api/s3/delete`.
S3 independently checks its actual running/storage state before deleting.
Never format automatically, delete on Start, or overwrite old files when full.

Initial file design: signed little-endian 32-bit samples containing the ADC's
24-bit value, a versioned header describing rate/format/session/part, and automatic
part rollover before a file grows to 1 GiB. Card wiring, filesystem mounting and
buffer capacity are verified during the S3 implementation. The existing filesystem
is preserved. A Feather/browser disconnect must not stop S3 recording.

## Existing acquisition protocol: status 3, commands 2

One status report per second, plus immediate command acknowledgements. Version 3
adds diagnostic counters. Acquisition remains on the S3. The 1,000 samples/second
hardware path and bidirectional Feather link worked on 2026-09-29; the new counter
reporting needs both firmware updates and a hardware check.

Upload Feather firmware and its web files first: it accepts both status versions
2 and 3. Then upload the S3. The older Feather rejects version-3 reports.
Commands remain `CMD,2` because their format and meaning have not changed.

## Wiring

115200 baud, 8 data bits, no parity, 1 stop bit; 3.3 V logic and common ground.

| Feather ESP32 V2 | S3 assignment |
| --- | --- |
| Header RX (GPIO7) | GPIO17 TX |
| Header TX (GPIO8) | GPIO18 RX for commands |
| GND | GND |

Feather uses Serial1 with RX/TX board constants, separate from USB Serial. S3
should explicitly assign RX=18/TX=17; these avoid digitizer GPIO9–13. Verify neither
has another role (e.g. a camera) before wiring. Monitoring uses S3 TX to Feather RX;
Start/Stop/Reboot commands require the reverse wire as well. This version is implemented
on Feather and S3; upload the matching S3 firmware before expecting controls to work.

Keep power and the analog DAC-to-digitizer wire separate from UART signals.
The previously discussed S3 5V -> Feather USB-pin supply can power Feather with the
USB cable on S3. Remove that 5V link before reconnecting USB power to Feather.

## Frame

ASCII CSV, exactly nineteen fields, LF or CRLF terminated. Both implementations
reserve 384 bytes per frame (including its terminator).

```text
S3,3,uptimeMs,adcReady,sampleCount,samplesPerSecond,latestRaw,sampleAgeMs,errorCode,running,appliedRate,ackId,ackResult,missedEdges,rejectedReads,readFailures,overlapReads,readyTimeouts,readFault
```

- uptimeMs: unsigned 32-bit S3 millis().
- adcReady: 0 or 1; 1 only after configuration readback and self-calibration succeed.
  TI does not specify a fixed chip-ID nibble; the driver does not assume ID=3.
- sampleCount: unsigned 64-bit count of conversions actually read from ADS1256 since
  counter reset. Never count heartbeats, DRDY edges alone, or failed reads as samples.
- samplesPerSecond: actual successful conversions per elapsed second, unsigned 32-bit;
  zero when stopped, not the configured target rate. Do not reject slight deviations
  above nominal 30000 caused by measurement-window or oscillator tolerances.
- latestRaw: signed 24-bit value (-8388608..8388607); send 0 before the first sample.
- sampleAgeMs: unsigned age of last successful conversion on the S3 clock;
  4294967295 means no sample yet.
- errorCode: 0 none, 1 initialization failure, 2 DRDY timeout, 3 ADC read/config error;
  other unsigned codes display numerically.

- running: 0/1 for confirmed acquisition state, separate from sample progress.
- appliedRate: confirmed configuration; one of 100, 500, 1000, 2000, 7500, 15000,
  30000. Default 1000 before ADC initialization. Never copy an unvalidated request here.
- ackId: last processed command id, 0 before any commands.
- ackResult: 0 no command, 1 successfully applied, 2 rejected. Repeat this pair in
  status reports until another command is processed; a dropped report must not lose it.

Heartbeat without initialized ADC: `S3,3,1000,0,0,0,0,4294967295,0,0,1000,0,0,0,0,0,0,0,0`

Acquisition example: `S3,3,5000,1,15000,1000,123456,1,0,1,1000,42,1,0,0,0,0,0,0`

## Commands: Feather to S3

`CMD,2,id,action,rate` followed by newline. id is a nonzero uint32. action is start,
stop, or reboot. rate is a supported integer for start, and 0 for stop/reboot.
Example: `CMD,2,42,start,1000`.

S3 must validate the entire command before acting. Start applies rate and starts
acquisition as one operation; while running, a different rate requires Stop first.
Stop and Reboot ignore draft settings. Stop must remain usable after an ADC error.
The S3 remembers the last sixteen completed commands until reboot. Within that
window, duplicate ids repeat the saved result without repeating the action; an id
reused with different contents is rejected. There is no durable replay history.
Stop acquisition before acknowledging Reboot, flush the acknowledgement
through the UART, then restart. Do not reboot before sending its status reply.

Feather sends once and exposes HTTP 202 plus commandStatus=pending, never immediate
success. Only a matching ackId with ackResult=1 AND the expected running/rate state
confirms the operation. Reject/mismatch becomes rejected. No response within five
seconds becomes timeout (outcome unknown); never retry Start/Reboot automatically.
Randomized id sequence per Feather boot reduces stale-ack collisions. Status still
reports actual device state even after timeout; a browser reload cannot erase a
pending command because command state lives on Feather.

The new Feather also accepts the exact thirteen-field version-2 status format.
Missing diagnostics become JSON null and display as Unavailable, never zero.
Version-1 and malformed reports cannot establish a connection or enable controls.

Use plain decimal integers and keep debug prose on USB Serial. No checksum in this
initial short-wire protocol: tag/version/field/range checks reject malformed frames
but cannot detect every altered digit.

## Freshness

Only complete valid frames refresh the three-second link timeout. Oversize or
expired partial lines are discarded without blocking or dynamic allocation.
'Receiving samples' requires count progress across recent frames and a recent ADC
conversion. A frozen count with a fresh heartbeat does not prove acquisition. A
steady ADC voltage is legitimate; the reading itself need not change.
Link timeout clears measurements to unknown instead of showing stale data as live.
The JSON API sends sampleCount and all diagnostic totals as decimal text to
preserve 64-bit precision. Disconnection makes these fields null; the browser
clears them if it loses contact with Feather as well.

Raw counts are not volts. S3 uses AIN0 minus AIN1, gain one, input buffer off, and
explicit self-calibration. The module's reference voltage and 7.68 MHz oscillator
assumption still need hardware verification. Advancing counts prove software read
activity, not correct analog wiring/calibration. Varying Feather's DAC and checking
ADC response is the subsequent physical validation.

## Run and fault behavior

S3 boots stopped at the default 1000 samples/s configuration. Count is cumulative
until reboot; Stop preserves count/latest reading and reports measured rate zero.
Start resets the throughput measurement window but not the cumulative count.
Feather disconnection does not stop acquisition. No files or full sample stream
are saved by this version.

A missing DRDY signal for 100 ms, failed configuration, or overlapping/incomplete
read stops collection and clears adcReady. Reboot S3 is the recovery path. Stop
still succeeds as a software stop if the digitizer is unresponsive; an ADC error
remains visible because hardware standby could not be confirmed.

## Diagnostic counters

All five new totals are unsigned 64-bit numbers, cumulative since S3 reboot.
Stop and Start preserve them. They describe what software observed, not an exact
count of all samples lost. There is no invented overall loss total or percentage.

| Field | Meaning |
| --- | --- |
| missedEdges | Extra observed Data Ready events between read attempts. Events preceding a rejected read are also counted. These indicate missed samples but can undercount loss if interrupts themselves are missed. |
| rejectedReads | Read attempts discarded because the driver failed its checks, another sample became ready during the read, or both. Each attempt counts once. |
| readFailures | Rejected attempts where the digitizer driver returned false. |
| overlapReads | Rejected attempts with a new Data Ready event observed during the read. |
| readyTimeouts | Incidents where no sample was ready for at least 100 ms and collection stopped. Counts incidents, not guessed missing samples. |
| readFault | Last rejection flags: 0 none since Start, 1 driver failed, 2 new ready event during read, 3 both. Start clears this detail, not the totals. |

readFailures and overlapReads are overlapping reasons for rejectedReads, not
additional losses. Never add those reason counts to each other or to rejectedReads
and present the result as lost samples. Rejected reads still stop acquisition.

The Feather's Acquisition Diagnostics panel displays these fields with plain
labels and explains the counting limits. Samples accepted and successful reads
per second remain in Digitizer Monitor. Unknown or stale measurements never show
as zero. Sampling throughput and loss at 30000 samples/s remain unverified.
