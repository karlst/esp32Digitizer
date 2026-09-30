# S3 acquisition status version 3; commands version 2

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
