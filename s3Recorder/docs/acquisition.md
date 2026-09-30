# S3 acquisition: first implementation

The S3 runs the digitizer. The Feather sends commands and displays a small status
report. It does not carry the sample stream. This version counts readings and keeps
the latest raw value; recording all samples is future work.

## Code map

| File | Responsibility |
| --- | --- |
| `main.cpp` | Start the worker and service the Feather link. |
| `ads1256.h/.cpp` | Own SPI sequencing, configuration readback, calibration, and reads. |
| `acquisition.h/.cpp` | Handle Start/Stop/Reboot and count successful conversions. |
| `acquisitionStatus.h` | Define the coherent status snapshot copied between tasks. |
| `commandProtocol.h/.cpp` | Reject malformed UART input and decode rates/signed samples. |
| `commandHistory.h/.cpp` | Remember sixteen recent results to avoid repeating commands. |
| `featherLink.h/.cpp` | Receive commands, report status, and acknowledge before reboot. |

## Why the task and lock exist

At the highest target rate, a new sample arrives about every 33 microseconds.
Formatting a status string or waiting for a UART would consume that time. A worker
on core zero owns the ADC and SPI exclusively; the Arduino loop handles UART on
core one. The GPIO interrupt only wakes the worker. It does not read SPI or print.

The control loop submits requests through a one-item FreeRTOS queue. The worker
performs them and publishes the result. This avoids two tasks changing ADC settings
at once. A full queue does not replace the earlier request or fabricate success.

The only shared record is a status snapshot. A short critical section protects its
copy, particularly its 64-bit count, which needs multiple operations on this 32-bit
processor. Otherwise a reader could combine halves of two different counts, or
show a new rate with old running state. The lock covers only a small memory copy.
It never covers SPI, calibration, waiting, text formatting, or UART output.

## ADC choices and assumptions

The driver follows [TI's ADS1256 datasheet](https://www.ti.com/lit/ds/symlink/ads1256.pdf).
It assumes the module's usual 7.68 MHz clock and uses SPI mode 1, at 1 MHz for
initialization and rates through 2000 samples/s, and 1.9 MHz for higher rates. It selects
AIN0 minus AIN1 at gain one, with input buffering and sensor-test currents disabled.
The clock output is unused. Every Start validates register readback and performs
self-calibration before continuous reads. The supported rates match the Feather menu.

At 100 through 2000 samples/s each conversion uses an explicit RDATA command;
the additional command and wait fit comfortably within the sampling period.
At 7500 through 30000 samples/s continuous-read mode avoids that per-sample overhead.
The original read-completion check failed after one or two readings because it
required DRDY high immediately when the S3 SPI controller finished. Real hardware
needed a short propagation allowance. The driver now waits at most three microseconds
for DRDY high. A stuck-low signal still fails; a new readiness edge during the read
still invalidates the sample. This change restored sustained 1000-sample/s operation.
The driver reads three bytes with zeroes on DIN so it cannot accidentally send a
stop/reset opcode. A read crossing another readiness edge is treated as ambiguous
and stops acquisition. Constant raw values, including zero and full scale, are
legitimate data and are not rejected merely because they repeat.

SPI has no acknowledgement or sample CRC. Configuration readback catches many wiring
faults, but cannot prove every data byte is correct. `ready` establishes successful
setup, not a known reference voltage or electrically verified analog input.

## Pins and startup

| S3 | Digitizer |
| --- | --- |
| GPIO9 | DRDY |
| GPIO10 | CS |
| GPIO11 | DIN |
| GPIO12 | SCLK |
| GPIO13 | DOUT |
| 3.3 V | PDWN (held high; no GPIO46 control) |
| 5 V / GND | 5 V / GND |

S3 TX17 connects to Feather RX; S3 RX18 connects to Feather TX, with common ground.
The analog test uses Feather A1 to AIN0 and AIN1 to ground. Verify the module's
reference/logic supply and board pin availability before electrical testing.

Boot leaves acquisition stopped. Loss of the Feather does not stop a running S3.
Faults stop collection, retain diagnostic evidence, and require Reboot S3 to retry
initialization. Reboot stops collection, transmits and flushes its acknowledgement,
then restarts. Read count survives Stop/Start and resets only on S3 reboot.

## Verification and remaining work

Build only: `platformio run -d s3Recorder`.
Desktop checks on Windows: `./s3Recorder/test/runNativeChecks.ps1` (installed MSVC).
Tests run actual parser, state, UART and driver code with substitute hardware;
they neither upload nor open serial ports. Outputs are under ignored `.pio/`.

Desktop checks cover malformed/expired frames, recovery at newline, numeric limits,
all seven rates, signed 24-bit limits, replayed commands, Start failure, Stop after
fault, cumulative counts, measured rate, missing DRDY, register mismatch, UART
congestion, and acknowledgement-before-reboot ordering.

The S3 firmware was uploaded on COM4 on 2026-09-29 with verified flash hashes.
Serial startup repeatedly reported ADC ready, stopped, target 1000, and error zero.
This verifies initialization, not live sample collection. Initial bench testing should use 1000 samples/s,
check status and analog response, exercise Stop/Reboot and disconnect behavior, then
increase rates. Actual 30000-sample/s throughput, missed events, core scheduling,
and watchdog behavior remain unverified. No watchdog is disabled to hide starvation.
`missedEdges`, reported over USB and version-3 status to Feather, counts observed
misses; zero is not proof of lossless acquisition. Feather also shows cumulative
rejected reads, driver-check failures, reads overlapped by a new ready event, and
data-ready timeouts. Totals survive Stop/Start and reset on S3 reboot. A rejected
read can have both failure reasons, so those reason counts must not be added.
See `shared/s3StatusProtocol.md` for the frame and update order.

The USB bench console also accepts the same strict `CMD,2,id,action,rate` commands.
It uses a separate bounded parser; plain debug text cannot become a command. This
allows automated Start/Stop tests without another browser click. It never starts
acquisition automatically. Avoid issuing USB and Feather commands simultaneously.

The existing generic S3 board configuration is retained (8 MB flash layout, PSRAM
unused); the recorded hardware has 16 MB flash/8 MB PSRAM. This version needs neither
the extra flash nor PSRAM. Storage work should resolve the exact memory configuration.
