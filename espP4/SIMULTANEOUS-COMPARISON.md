# Simultaneous input/output waveform comparison without SD

Karl completed PC checks, build/upload and simultaneous captures on October 18.
See [results](../docs/STATUS-2026-10-19-P4.md) and [current firmware choices](CURRENT-SETUP.md).
The current reader requires DAC MODE identification; rebuild older Feather
comparison firmware before using it. A successful capture is not ADC calibration.

## What is being compared

The existing distorted signal remains the test input. Keep the Feather A1->A2
jumper, DAC coax to TI CH0P, CH0N grounding and common ground as established.
Both boards and the ADC must be powered normally. Make wiring changes only
while unpowered. No analog changes are proposed for this experiment.

Feather's ADC measures physical A2 voltage while the ADS127/P4 chain measures
the same connected source. Feather uses the same benchDac code and VCM=0x04
configuration already tested. This is not a comparison against calculated sine
codes or an earlier run. Both records carry the same run ID from the handshake.

Feather now collects a nominal 500 us burst every millisecond after one second
of settling, saving the mean, minimum, maximum, conversion count, actual burst
duration and burst midpoint time relative to the confirmed START command echo.
Each call to adc1_get_raw contributes one reading. Calls are not uniformly spaced;
this arithmetic mean approximates rather than exactly reproduces P4's filter.
Core 1 of P4 retains the existing 600 half-millisecond bins
of mean/minimum/maximum signed-24-bit ADS127 counts. Its time origin is decoded
sample position at the requested rate, not Feather's software clock. The memory
image is therefore waveform summaries, not every full-rate sample. USB transfers
happen after stopping; no SD preparation, mounting, writing or readback occurs.

The Feather sampler is owned by Arduino loop and does not run in the DAC timer
callback. During the 300 ms measurement interval, loop polls with 50 us delays
instead of a full RTOS tick between polls, so 500 us bursts can fit into nominal
1 ms slots. The higher-priority DAC timer and interrupts remain enabled. This
adds diagnostic CPU work on Feather; it does not establish production headroom.
Its bounded 320-point global array is reset between runs. It exports
only after ADC stop, UART STOP to P4, and DAC disable. A local ADC driver/capacity
failure requests stop; partial records are marked unclean. The capture reader
requires both records clean, complete and from the same run before comparing.

## Timing, scale and what the result can establish

The PC uses input points at 20..100 ms to fit only three quantities:

- Time shift within +/-20 ms, searched in 0.025 ms steps.
- One linear gain converting P4 counts to Feather-count units.
- One constant offset in Feather-count units.

Positive time shift means evaluating P4's trace later on its own timeline to
match a Feather measurement. These origins are not hardware synchronized, so
the fitted shift is NOT a calibrated ADC latency measurement. No clock-rate
warp, per-cycle adjustment or nonlinear remapping is allowed. Hitting the search
boundary or obtaining a negative gain produces a warning, not silent success.

Those three values are fixed for comparison at 100..280 ms. The report shows
both raw traces separately, the aligned/scaled overlay, and residuals on points
not used to fit. CSV retains every compared point, interpolated P4 value, adjusted
value, difference and whether it was used for fit or validation. RMS and maximum
absolute differences are in Feather counts; normalized RMS uses measured input
peak-to-peak amplitude, NOT the ADC's full-scale range. No arbitrary PASS limit.

P4 bin means are linearly interpolated to Feather measurement times. Its uniform
512-sample mean is not identical to a burst of asynchronously timed Feather reads.
The report warns about burst durations materially different from 500 us. DAC steps,
ADC filtering, Feather noise/nonlinearity and independent clocks can produce
residuals even if digital capture is correct. This test assesses waveform-shape
agreement at this resolution. Fitting gain/offset removes evidence of constant
gain/offset error; it cannot establish voltage calibration, every full-rate
sample's correctness, or 24-bit converter precision. Serial report count/order/
run ID validation is not a payload CRC. Original logs preserve P4 extrema and
all diagnostic messages for later inspection.

## Averaging experiment and production boundary

The initial simultaneous run 3173507255 used individual Feather readings. Both
records were clean, but validation RMS difference was 62.039 Feather counts
(11.772% of measured input peak-to-peak), with a maximum of 169.790 counts.
The input measurements were much more jagged than the independent loopback.
Do not assume those excursions are measurement noise rather than real signal.

Karl authorized Feather burst averaging as the next diagnostic comparison, with
production suitability to be verified separately. New INPUT headers declare
mode=burst; the parser remains compatible with old single-reading logs. The raw
input plot retains pale min/max traces, and comparison-input-bursts.csv preserves
means, extrema, counts and durations. USB collection and P4 firmware are unchanged.
Compare absolute RMS as well as normalized RMS: averaging can change the measured
peak-to-peak denominator, so percentage improvement alone is not comparable.

This change does NOT average the full-rate production SD payload. It only changes
the optional Feather measurement in ads127-p4-compare. Production acceptance needs
an agreed bandwidth, shortest event to preserve, amplitude/timing error limits,
and a suitable independent measurement reference. Averaging can suppress real
short events as well as noise. A reduced difference on this 10 Hz experiment does
not approve averaging, analog accuracy or CPU capacity for production.

## Repeat this experiment

From repo root, first run the PC-only math/protocol checks (standard library):

```powershell
python .\espP4\test\comparisonChecks.py
```

Build Feather's new optional environment:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run --project-dir .\espFeather -e ads127-p4-compare
```

After a successful build, close COM4 monitors and upload using the same command
plus `-t upload --upload-port COM4`. The current P4 RAM firmware is unchanged;
no P4 rebuild/upload is needed. This Feather build replaces the standalone
loopback build and once again controls the ADS127 through the established SPI
and inter-board UART wires. DAC stays disabled at boot.

With both boards/ADC powered, close BOTH COM3 and COM4 monitors, reset P4, then:

```powershell
python .\espP4\test\captureComparison.py --feather COM4 --p4 COM3
```

The reader opens both ports, requests Feather help and selects P4 RAM mode with
w. Only after COMPARE READY and RAM READY does it send Feather 6. No manual p
or 6 is needed. Two independent PC reader threads drain USB concurrently so one
board's report does not block the other's. Expect a roughly 1.3 s acquisition,
then report transfer, analysis, and a printed comparison.html path under
captures/waveform-comparison/<timestamp>. That folder also holds both original logs
and comparison.csv. Closing the reader is not hardware confirmation of stop;
on interruption it attempts Feather s, and firmware retains bounded timeouts.

If collection fails, logs remain and analysis refuses the incomplete capture.
Reset P4 before retrying. To reanalyze a saved pair without boards:

```powershell
python .\espP4\test\compareWaveforms.py <feather.txt> <p4.txt> --output <comparison.html>
```

The scripts do not build, upload or modify card files. Karl performs those steps.
