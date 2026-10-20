# Paired RAM capture text protocol

Read captureComparison.py first for PC orchestration, InputSnapshot.cpp for
Feather measurement/export, and RamWaveform.cpp plus main.cpp for P4 collection.
These are diagnostic USB reports, not the SD file format or a production API.

Each serial reader assembles newline-terminated text and queues lines for the
main PC thread. The PC saves complete per-board text logs before analysis.
The two USB links do not provide a shared sampling clock. A common run ID ties
reports to the same requested experiment, not to exactly simultaneous readings.

## Preparation and completion

PC sends Feather `h` and P4 `w`. It waits for `COMPARE READY:`, `DAC MODE:` and
`RAM READY:` before sending Feather `6` (1,024,000 requested samples/second).
`--constant` requires `DAC MODE: CONSTANT code=135 ...`; the sine comparison
uses the other mode. Older firmware without DAC MODE will time out: rebuild the
matching Feather environment. P4 refusing mode selection requires a P4 reset.
Do not leave another monitor holding either port.

Feather starts only after the P4 UART READY handshake. Once the P4 has its RAM
window, RAMFULL requests Feather stop; the P4 drains decoding before printing.
The PC waits for both INPUT_END and WAVE_END. Capture timeout/parse failures retain
logs. An incomplete PC session requests Feather `s`; firmware timeouts also exist.
USB closure alone never establishes that the ADC stopped.

## Feather fields

`INPUT_CAL run=... source=... defaultVrefMv=1100` identifies the SDK calibration
source: 0 factory reference, 1 factory two-point, 2 nominal fallback, 3 fitted
two-point. The 1100 mV value is an SDK fallback argument, not the DAC supply or
the TI ADC reference. The returned source tells whether it was used.

`CAL,raw,millivolts` maps raw 12-bit ADC counts to approximate mV, every 32 counts
plus 4095. SDK characterization and export happen after capture stops. The PC
interpolates the integer-mV table; it does not infer calibration from the sine fit.

`INPUT_BEGIN run=... count=... clean=... missed=... maxGapUs=... mode=burst`
declares the record. `INPUT,index,timeUs,mean,min,max,reads,durationUs` follows.
Time is the burst midpoint relative to Feather's observation of ADC START echo;
the first second is settling. Mean is the sum of raw readings divided by reads.
Min/max are individual raw readings within the burst, not uncertainty bounds.
Duration includes scheduling delays. Slots begin nominally every millisecond;
each burst lasts approximately 500 us. `INPUT_END run=...` terminates the record.

## P4 fields

`WAVE_BEGIN run=... rate=... bins=600 samplesPerBin=... clean=...` precedes
`WAVE,index,min,max,mean` rows and matching `WAVE_END run=...`.
Values are signed 24-bit counts. At stage 6, each bin combines 512 consecutive
decoded samples, nominally 0.5 ms. P4 skips one second before collecting 300 ms.
The PC derives bin midpoint time from the index, not USB arrival time.

## What the analysis accepts

Parsers require matching run IDs, ordered rows, expected counts/ranges, end
markers and clean flags. These checks are not a checksum of the USB payload or
proof that every ADC sample arrived. Sine reports fit timing/gain/offset; constant
reports do not. Constant reports center each converted trace on its own mean,
with the same mV axis bounds, and retain separate raw count plots below.

Old raw-only records remain readable, but cannot produce a calibrated Feather
voltage plot without the missing calibration data. ADC filtering, unequal
averaging counts and approximate voltage conversion limit comparisons.
