# Recording and test source layout

The application implementation is shared by recording and tests. Only startup
and the services supplied at startup differ; there is no copied decoder, writer
or ADC configuration sequence.

The FSYNC startup additionally supplies `FsyncTimedTest` for explicit console
`t` / `--timed-sd-test` selection. It delegates real ADC/decoder control and
passes observed boundaries to `TimedRecorder`. Its reusable format/staging code
lives under `lib/timedRecording`; only `TimedRecorder` knows the P4 task/memory
and disk adapter. `timedIntervalChecks.cpp` tests staging and exact sample order;
`timedCaptureChecks.py` tests the PC's completion-count validation. The existing
console `d` sample-only SD experiment remains unchanged and selectable after reset.

| Purpose | P4 | Feather |
| --- | --- | --- |
| Ordinary startup | `main/src/main.cpp` | `src/main.cpp` |
| Real application | `main/src/RecordingApplication.cpp` | `src/AcquisitionApplication.cpp` |
| Console/Feather controller | `main/src/RecordingController.cpp` | Part of `AcquisitionApplication` |
| Receive and decoding tasks | `AdcReceiver`, `SampleDecoder` | ADC control remains on Feather |
| Terminal reporting | `ConsoleRecordingReporter` through `RecordingReporter` | Part of `AcquisitionApplication` |
| Application headers | `main/include` | `include` |
| Test startup | `test/hardware/main.cpp` | `test/hardware/ads127CaptureControl.cpp` |
| Test services | `RecordingHarness`, `RamCaptureDestination`, `SdWriteTest`, `SdSmokeSummary` | `BenchAcquisitionServices`, `BenchDac`, `InputSnapshot` |
| Profiling | `test/profile` | No separate CPU profiler |

P4 CMake selects dedicated test startups for smoke, profiling,
`CONFIG_P4_DIAGNOSTICS`, FSYNC/pulse-delay, and input-skew configurations.
Ordinary startup creates no test objects or generated-data task. The recorder's
layout and readback interfaces allow test-specific verification without including
test classes in the recorder. `CONFIG_P4_DIAGNOSTICS` enables manual RAM and SD
speed tests in a separate configured build; smoke/profile already include them.

`RecordingHarness` implements separate command and observation interfaces.
`RecordingObserver` has no command methods. `espP4/lib/bufferedWriter` owns
the ring implementation and independent writer tests. See
[Recording classes](../../docs/RECORDING-CLASSES.md) for the control/data split.

Feather environment `recording` selects ordinary startup and the external ADC
input. The existing named test environments select their own startup and inject
test services. The reusable ADC configuration and SPI driver live in `src/adc`
and `include/adc`. `common/BenchLine.h` assembles protocol lines for both boards;
shared test-duration choices remain in `common/test/support`.

The preserved `espFeather/webApp` is a separate PlatformIO project. Its LED
service lives with its own source and headers; terminal firmware does not link
web code. Its receiver-status interface has not been converted into P4 control.
Feather driver checks and Arduino/SPI substitutes live in `espFeather/test`.

[Data flow](../../docs/DATA-FLOW.md) describes ownership and actual transfer calls.
[Build instructions](../../docs/BUILDING.md) list ordinary and test builds.
After source changes, Karl runs PC checks and firmware builds before uploads;
source inspection alone cannot establish compilation or hardware behavior.

## Reading the timing tests

Start with the test you want to understand. All three collect in RAM and report
after capture; none writes an SD recording or implements the proposed timing
block file format.

| Question | Firmware entry point and measurement | PC runner and analysis |
| --- | --- | --- |
| Which ADC word belongs to which FSYNC? | `fsync/main.cpp`, `FsyncAssociationTest.cpp`, `FsyncAssociationAnalysis.cpp` | `runFsyncAssociation.py`, `fsyncAssociationReport.py` |
| How long is the analog-input-to-FSYNC path? | Same startup with `PulseDelayCapture.cpp` added | `runPulseDelay.py` delegates acquisition to `runFsyncAssociation.py`; `pulseDelayReport.py` calculates delay |
| How differently do two P4 inputs timestamp the same pulse? | `inputSkew/main.cpp`, `InputSkewTest.cpp` | `runInputSkew.py`, `inputSkewReport.py` |

For the first two, read startup, `prepare`, `submit`/`takeSnapshot`, then `finish`.
Core 1 decodes ADC words into the test's RAM. Core 0 reads occasional FSYNC
observations and requests shutdown. `finish` runs after decoding drains, so its
word/time comparison does not race a changing sample array. The pulse-delay
variant also records generator transitions in an interrupt callback, sharing
the same hardware timer as FSYNC. Its PC analysis uses both the sample-to-FSYNC
mapping and the direct generator timestamp.

The input-skew startup is independent. It does not construct the receiver or
control Feather. It compares two copies of the same signal, not ADC input and
output. A positive difference means GPIO1 recorded the edge later than GPIO6.

### Terms and units

- **Sample:** one ADC voltage reading. The test retains a four-byte word with
  status and a signed 24-bit reading; an eventual recording can pack the reading
  into three bytes.
- **DCLK:** serial data clock; each clock moves the ADC frame along.
- **FSYNC:** frame synchronization signal marking a serial frame. The tested
  configuration uses 32 clocks per frame and 1,024,000 samples per second.
- **Edge:** a transition from LOW to HIGH (rising) or HIGH to LOW (falling).
- **Hardware capture:** the peripheral saves its timer count at an edge. Time
  spent waiting to execute software does not become that captured timestamp.
- **Timer tick:** nominally 6.25 ns at 160 MHz. This is different from a FreeRTOS
  scheduler tick, which is used to yield CPU time to other tasks.
- **Anchor:** one accepted pulse-number/timer-count pair. Not every FSYNC
  timestamp is kept. Hardware counts all pulses; software saves occasional pairs.
- **ISR:** Interrupt Service Routine, a callback invoked by the driver. The
  direct-input tests copy already captured timestamps into bounded arrays there.
- **DMA:** Direct Memory Access, which transfers received bytes in batches.
  A final undelivered batch is reported as a bounded tail at shutdown.
- **Residual:** difference between an observed timestamp and its prediction
  from the line joining first and last anchors. Small residuals show consistent
  spacing, not an independent absolute-time calibration.

### Commands, evidence and checks

The PC asks `h` for identity/readiness before sending `g` to start a run.
Association/delay uses Feather's real ARM/READY/START/STOP/DONE sequence; the
standalone skew image handles `g` locally. A queued command is not confirmation
that hardware changed state. `runFsyncAssociation.py` waits for both a validated
P4 report and Feather's matching DONE. Its first-run `@LINK` probe checks the
separate board-to-board UART link using a numbered request and response.

Raw `p4.txt`/`feather.txt` logs preserve evidence. `result.json` holds calculated
results; `comparison.html` draws them. Delay reports remain raw measurements
relative to GPIO6: they do not automatically subtract the separately measured
input skew or the agreed 2553-ns recording correction.

`decoderChecks.cpp` tests startup discard and counter arithmetic.
`fsyncAssociationReportChecks.py`, `pulseDelayChecks.py`, and
`inputSkewChecks.py` test the PC interpretation with synthetic observations.
`test/runSmokeChecks.ps1` runs these alongside the existing PC suites; it does
not build or upload board firmware. Read the corresponding procedure in
`docs/FSYNC-ASSOCIATION-TEST.md`, `docs/PULSE-DELAY-TEST.md`, or
`docs/INPUT-SKEW-TEST.md` before a physical measurement.


## FSYNC read consistency experiment

The existing FSYNC firmware also accepts `r` while idle to select a ten-second
read experiment. Reset the board to return to ordinary association mode.
`runFsyncAssociation.py --read-test --runs 1` selects it explicitly and waits for
`READ MODE version=3` before asking Feather to start. Use the ordinary external-
source Feather firmware; no PPS or generator signal is required. Keep GPIO1
connected to translated ADC FSYNC. This test does not write to SD.

Read `fsync/FsyncReadTest.h` and `.cpp` for the measurement. Core 0 calls it once
per application poll (roughly 1 ms), after decoded samples establish activity and
before the ten-second target. A varying 0..31 us delay changes observation phase.
Core 1 continues normal decoding; only the first 65536 words are retained in RAM.
It does not allocate ten seconds of sample storage. Shutdown still uses the real
Feather handshake and checks decoder/receiver integrity.

Each attempt reads count/time then count/time. Exactly one changed field is an
inconsistency. Both changed or neither changed is accepted only if the measured
bracket is shorter than one FSYNC period: fewer than 352 CPU cycles at 360 MHz.
Up to 32 fresh comparisons are allowed per set. HIGH is a label, not a failure.
Repeated equality/change is the tested rule, not proof of same-edge association.

`READ LEVEL` splits attempts by LOW, HIGH, or differing levels at the endpoints.
It reports inconsistent comparisons, slow comparisons, and their union (rejected).
Reasons overlap: do not add inconsistent and slow to obtain rejected.
`READ TOTAL` reports exhausted sets, all attempts, and attempts per set.
`READ CONSISTENCY` additionally isolates sets where all 32 were inconsistent,
so a slow but consistent comparison is not mislabeled inconsistent.
`READ TIME` gives minimum/mean/maximum comparison and whole-set nanoseconds.
Comparison brackets include four register reads and two boundary fences, but
exclude GPIO-level reads, comparisons, and critical-section entry/exit. Whole-set brackets include
locks, retry bookkeeping and intervening interrupts, but exclude trigger jitter.
Empty-bracket overhead is reported separately and is not subtracted. These are
observed times under receive/decode load, not guaranteed worst-case times or a
disk-load test. `READ RESULT MEASURED` means the acquisition completed cleanly and
observations exist, not that no read attempts failed.

From an ESP-IDF PowerShell in `espP4`, build with:

```powershell
idf.py -B build/fsync -D SDKCONFIG=sdkconfig.fsync -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.fsync.defaults" build
```

After a successful build, upload with `idf.py -B build/fsync -p COM3 flash`.
Reset both boards and close serial monitors before running:

```powershell
python .\test\runFsyncAssociation.py --read-test --runs 1
```

Default ports remain P4 COM3 and Feather COM4. The runner saves raw console logs
and the READ report in result.json under captures/fsync-association. It neither
builds nor uploads firmware. The original association and pulse-delay commands
retain their original short captures when read mode has not been selected.


Read mode also reports eight `READ COST` rows: CPU-counter overhead,
an empty two-fence bracket, an empty five-fence bracket, one pulse-count read,
one captured-timestamp read, one pair, two pairs with boundary fences only,
and two pairs with three additional inter-read fences. Each has its own short
critical section; lock entry/exit, GPIO-level reads, comparisons and statistics
are outside these brackets. Every case runs once per observation during normal
ADC reception and decoding. In version 3 the shortened retry experiment follows
these timings. Its `READ TIME` row excludes GPIO-level reads and the three
inter-read fences; comparisons and statistics remain outside the bracket.

`READ COST` gives raw CPU-cycle and nanosecond minimum/mean/maximum durations.
`READ ESTIMATE` subtracts the matching empty-bracket mean (two or five fences).
These differences are estimates, not an exact separation of bus read and fence
cost: a fence can wait longer when peripheral work is outstanding. Boundary-only
cases are latency experiments, not evidence that a pair belongs to one FSYNC.
The compiler is required to retain returned values before the ending counter
read; no value comparisons or RAM stores are performed inside those brackets.
The test completion label is now MEASURED rather than PASS to avoid implying
that read acceptance succeeded when the acquisition itself completed cleanly.


Version 3 requires a matching runner acknowledgement so older firmware cannot
silently supply results from the longer retry sequence. The four volatile reads
remain count/time/count/time. Espressif's ESP32-P4 chip revision v1.3 TRM,
section 1.6.5.2 (page 99), states that the architecture guarantees memory ordering;
inter-read fences are therefore omitted. Two boundary fences still delimit CPU
cycle timing. Each comparison must take fewer than 352 cycles and have either
both fields changed or neither changed. GPIO HIGH/LOW labels refer to observations
outside the bracket; they are not proof of the level throughout it. Neither
successful timing nor this comparison rule independently proves same-edge
association. No file-format or production capture behavior changes in this mode.


## Timestamp-derived sample numbering experiment

`runFsyncAssociation.py --index-test --runs 1` selects `INDEX MODE version=2`
in the existing FSYNC build. It uses the same ten-second acquisition and
external-source Feather firmware, with GPIO1 still connected to translated ADC
FSYNC. No PPS, analog generator, or SD write is required. Read
`fsync/FsyncIndexTest.h` and `.cpp` for the implementation. Rebuild and upload the
FSYNC firmware using the commands above, reset both boards, close monitors, then
run from espP4:

```powershell
python .\test\runFsyncAssociation.py --index-test --runs 1 --p4 COM3 --feather COM4
```

The startup sequence clears PCNT while the ADC is stopped, confirms no pulse
arrived during receiver setup, and discards exactly one initial serial frame.
The established mapping is pulse 2 -> stored sample 0, pulse 3 -> sample 1.
The first usable timestamp/count observation therefore identifies a later sample
unambiguously IF that pair is correct. For example, pulse 3614 maps to sample
3612, with its actual captured timestamp. The test does not pretend it directly
captured the timestamp of pulse 2 or fabricate a sample-zero timestamp.

At roughly one-millisecond intervals, read a captured FSYNC timestamp. Divide
the change since the previous timestamp by the nominal 156.25 ticks per sample,
round to a whole number, and advance the calculated pulse number. PCNT never
repairs the prediction. These frequent observations are required by this
alternative even though PPS is once per second. PPS handling is not implemented.

For comparison, read count/time/count/time. Require unchanged counts and
unchanged timestamps within less than one FSYNC period. The PCNT driver supplies
an accumulated count before that bracket; extendCount attaches the raw register
to it, accounting for 32767-pulse wraps. Require the driver baseline to be at most
100 us old. Moving, slow, or stale observations cannot check the prediction.
After initialization they still advance it using their first timestamp. There
is no retry loop. All valid comparisons now use full counts, not just a modulus.

The initial pair is not an atomic hardware latch. To check its offset, version 2
also waits until the source has stopped and the timing hardware is frozen. It
then reads the final timestamp and counter, which are both stationary. Extend
the prediction from its last observation to this final timestamp and require
exact agreement with the final count. The final count never adjusts the
prediction. A fixed initial count error is therefore visible at the endpoint
unless another error cancels it; the running comparisons provide additional
evidence against that case. Source-stop, decoder continuity and DMA-tail checks
must also pass before accepting the run.

The first 64 predicted anchors can be associated with the first 65536 retained
words after decoder drain. FsyncAssociationAnalysis checks their spacing, and
the report indexes the actual RAM words with pulse-2. This verifies the software
mapping under the previously established startup rule; sample values alone
cannot independently prove their own physical pulse number.

`INDEX ENDPOINT` reports predicted and settled final counts and their difference.
`INDEX START` combines the clean-start checks, endpoint check, retained-anchor
spacing, and absence of running disagreements. `INDEX REFERENCE` identifies the
starting timestamp's sample index and actual retained word; it also prints word
zero. `INDEX SAMPLE` lists up to eight later sample/timestamp/word associations.
`INDEX TOTAL` reports matches, disagreements and unusable references (reasons
may overlap). `INDEX MODEL` reports pulse increments and rounding margins.

A rounding residual of zero is an exact whole number of sample periods; 0.5 is
the rounding boundary. Prediction stops if a gap exceeds 10 ms or a residual
reaches 0.25 sample, leaving a quarter-sample guard. These are diagnostic limits,
not established oscillator-accuracy guarantees. There is no silent restart from
PCNT. The stopped endpoint has the same tick-gap and rounding guards.

`INDEX RESULT status=MEASURED` means acquisition completed cleanly and comparisons
exist. `agreement=YES` additionally requires `INDEX START` to pass. This establishes
evidence for sample numbering in this run under the checked startup convention;
it does not test GPS/PPS association, SD loading, or long-term behavior.
Raw P4/Feather logs and INDEX rows in result.json are saved by the shared runner.


## PPS-to-numbered-sample experiment

The same FSYNC firmware now supports `--pps-test --runs 1`, acknowledged as
`PPS MODE version=2 seconds=60 gpio=6`. Rebuild/upload before using this mode.
It runs sample numbering for sixty seconds, including startup and stopped-endpoint checks,
with GPIO6 rising-edge capture on the exact same 160-MHz timer as GPIO1 FSYNC.
The alternative pulse-delay firmware owns GPIO6 itself and refuses PPS mode.

Read `fsync/PpsSampleTest.h` and `.cpp`. The driver ISR copies the hardware-latched
PPS tick count into an 80-entry internal array. Core 0 attaches pending PPS events
to the first subsequently observed, numbered FSYNC. The two timestamps are real
hardware captures; no interpolation of an unobserved nearest FSYNC is performed.
The reference sample will ordinarily be within about one millisecond after PPS,
not necessarily the next sample. Mapping over 10 ms late invalidates the run.
The timestamp predictor still needs frequent observations between PPS pulses.

Hardware preparation: preserve ADC DCLK/FSYNC/DOUT0, translated FSYNC to GPIO1,
and the Feather UART. Power the ADC and the GPS. Connect the GPS's previously
measured approximately 3.3-V PPS output to P4 GPIO6, with GPS/P4 ground shared.
Disconnect any generator output from GPIO6 before making that connection. PPS
is not routed through ADC CH0 and requires no analog resistor divider. A 1-Hz
generator can exercise capture, but it cannot establish GPS timing accuracy.
Verify which source is actually being used before interpreting the results.

After wiring, reset both boards and close serial monitors, then from espP4 run:

```powershell
python .\test\runFsyncAssociation.py --pps-test --runs 1 --p4 COM3 --feather COM4
```

The runner waits for both boards and the real acquisition-stop handshake. No
SD files are written by the boards; PC raw logs and result.json retain the report.
`PPS INTERVAL` prints differences between successive hardware captures, expected
near 160 million ticks. The diagnostic accepts +/-1 percent around one second;
that is a gross missing/extra-pulse check, not a calibrated GPS-accuracy bound.
`PPS SAMPLE` identifies a zero-based decoded sample number, its observed FSYNC
timestamp, and the PPS timestamp. Subtract those counts to get FSYNC-after-PPS
in ticks. Nominal nanoseconds are ticks times 6.25. `inputAfterPpsNs` then
subtracts the agreed 2553-ns digitizer/translator correction. No new calibration
of cable delay, GPIO skew, oscillator scale, or GPS error is implied.

Sample numbering must pass the stopped-endpoint and retained-word startup
checks before a PPS run is accepted. Every reported sample index must be inside
the fully decoded stream. Only the first 65536 sample values are retained in RAM,
so most PPS-associated values are not themselves retained by this diagnostic.
`event` numbers PPS captures from zero; it is NOT a UTC second number.

`PPS TOTAL` distinguishes pulses before the first numbered observation, pending
pulses after the final observation, too-late mappings, invalid intervals and
capture overflow. Require 59..61 mapped pulses and 59..61 captured pulses during
the sixty seconds, no overflow, no late mapping, and no invalid interval. At least
two timer wraps must be observed. The one-pulse count tolerance accommodates
source phase and the short startup/shutdown windows; interior gaps or extra
pulses are still rejected by the interval check. Captures outside the
numbered window are reported and excluded. Input absence, extra fast edges,
lost numbering or acquisition faults yield `PPS RESULT status=FAIL`.

`PPS RESULT status=MEASURED` establishes this capture/mapping experiment only.
It does not read GPS serial messages, confirm GPS lock, associate a UTC date or
second, construct recording tBlocks, or validate sustained SD throughput.


The 60-second PPS mode stops after 61,440,000 decoded words; retained sample
storage stays at 65,536 words. Other read/index tests remain ten seconds. The
P4 diagnostic watchdog is 64 seconds and its outer acquisition deadline is
65 seconds. The PC waits up to 90 seconds for acquisition and report transfer.
Existing Feather version-6 firmware supports an idle `@TEST 300` command, so
the runner confirms that safety timeout before starting; the P4's normal
RAMFULL/STOP handshake stops the ADC at sixty seconds. No Feather reflash is
needed. The Feather duration stays at 300 until reset or another @TEST command;
reset both boards before changing tests. If the PC aborts, its existing cleanup
sends the Feather stop command; the longer Feather timeout is only a fallback.

The old PPS leading-edge test compared every event to the first FSYNC using
32-bit subtraction. That comparison becomes ambiguous after half a timer range
(13.42 seconds) and has been replaced. FSYNC time is now extended by accumulating
short, modulo-32-bit differences between polls into a 64-bit total. Each pending
PPS is compared to the nearby current FSYNC, then assigned that extended epoch.
`ppsTicks64` and `fsTicks64` stay continuous across wraps. Their difference is
still the directly captured PPS-to-FSYNC interval. `PPS WRAPS` reports detected
wraps and observation duration. Success requires at least two wraps, independently
of the ordinary sample-number and stopped-endpoint checks. This is still a RAM
experiment; sustained SD throughput remains untested.


## Combined PPS timing and SD throughput experiment

`--pps-sd-test --runs 1` selects the explicit `PPS SD MODE version=1` command in
FSYNC firmware. Default diagnostic commands still use RAM; neither startup nor
the link probe mounts or writes the card. Reset before selecting the combined
mode and before returning to a different test. The pulse-delay firmware refuses
this mode because its GPIO6 capture is already occupied.

Read `fsync/FsyncSdTest.h/.cpp`, then the existing SdCaptureDestination and
SdRecorder. The wrapper composes the real destinations rather than implementing
another recording path. The original observer still supplies startup alignment
and FSYNC/PPS capture. Core 0 asks the recorder to prepare one new 65-second file
(addFiles, stopWhenFull), preserving the card format and all previous files.
Only after SD readiness does it arm timing hardware; PPS arrays and the capture
deadline therefore exclude preallocation. Core 1 sends each ordinary decoded
batch through the existing three-byte packer and 16-MiB ring. The real Core 0 disk
task pumps 128-KiB chunks using the existing underlying write calls. The original
small retained sample window remains for startup verification. Refused submissions
or receiver errors invalidate acquisition; there is no retry of dropped data.

The file reserves about 200 MB, with about 184.32 MB of sample payload expected
for sixty seconds plus shutdown tail. It is the CURRENT recording format, not
the proposed header/tBlock/sBlock design. Timing records remain in the PC log.
No format, overwrite, or deletion operation is selected. An unformatted/non-exFAT
card or insufficient space must fail preparation rather than erase anything.

Hardware used for the successful run: the Rigol supplies GPIO6 directly and
CH0P through the verified resistor divider; CH0N remains grounded. The generator
is 1 Hz, 10% duty, measured 0..3.3 V before the divider, with shared ground.
The loaded divider keeps CH0P within range. A grounded CH0P is also adequate
for a throughput-only test. FSYNC remains on GPIO1 through the translator. Install the existing
project SD card, power up, reset both boards, and close monitors. Build/upload
P4 FSYNC firmware, then from espP4 run:

```powershell
python .\test\runFsyncAssociation.py --pps-sd-test --runs 1 --p4 COM3 --feather COM4
```

Only one combined run is accepted per invocation. The runner first sets P4
calendar time from the PC and requires UTC OK for the existing file-header date;
this does not establish GPS time. Feather's existing @TEST 300
safety timeout is confirmed; P4 ends acquisition after sixty seconds. Allow time
for file preparation before acquisition and full CRC readback afterward. The PC
allows up to eight minutes for all phases and prints storage progress. Keep the
card inserted through `SD SAVED` or `SD FAILED`, even after Feather reports DONE.
A PPS-result failure still waits for SD completion so both results are retained.
No Feather firmware change is needed.

A combined pass requires the same sample-zero, prediction, PPS interval and timer
wrap checks, plus SD SAVED, exact equality of saved bytes to three times decoded
sample count, CRC-verified retention of the complete stream, one new file, no
file reuse, no lost bytes, no buffer overflows, no write errors and no unsaved
bytes. Report ringPeak and maxWriteOrFlushUs in result.json for comparison with
the earlier throughput measurements. A finite successful run demonstrates this
workload; it does not establish a worst-case SD-card latency guarantee or GPS UTC.

The November 7 combined run passed after the decoder inline-path repair. See
[the diagnostic guide](../../docs/FSYNC-ASSOCIATION-TEST.md) for the saved result,
the regression explanation, and definitions of the retained timing counters.
