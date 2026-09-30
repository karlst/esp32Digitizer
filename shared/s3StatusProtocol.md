# S3 acquisition status and commands, version 2

One short report per second; acquisition stays on S3. Both firmware projects now
implement this protocol. S3 upload on COM4 and startup/configuration checks passed
on 2026-09-29; live acquisition and the Feather link remain untested. Commands also
trigger immediate replies.

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

ASCII CSV, exactly thirteen fields, LF or CRLF terminated, at most 159 characters:

```text
S3,2,uptimeMs,adcReady,sampleCount,samplesPerSecond,latestRaw,sampleAgeMs,errorCode,running,appliedRate,ackId,ackResult
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

Heartbeat without initialized ADC: `S3,2,1000,0,0,0,0,4294967295,0,0,1000,0,0`

Acquisition example: `S3,2,5000,1,15000,1000,123456,1,0,1,1000,42,1`

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

This is a coordinated protocol upgrade. Version-1 status is rejected and cannot
enable controls. Both boards now have matching firmware; the inter-board link
still needs wiring and testing.

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
The JSON API sends sampleCount as decimal text to preserve 64-bit precision.

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

USB diagnostics include missedEdges, a lower-bound indication of observed DRDY
events overwritten before reading. This is not an exact lost-sample counter: edges
can themselves be missed. The Feather v2 frame remains thirteen fields and shows
actual successful reads/second. Sampling throughput, scheduling/watchdog behavior,
and loss at 30000 samples/s must be measured on the real hardware.
