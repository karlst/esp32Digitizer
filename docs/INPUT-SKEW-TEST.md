# Same-pulse P4 input timing comparison

Purpose: measure whether GPIO1 records a shared pulse earlier or later than
GPIO6. GPIO1 previously received translated ADC FSYNC; GPIO6 received the direct
generator branch. This test excludes the ADC and translator from both signal
paths. It does not measure either input's absolute detection latency.

## Wiring and power

Keep AFG output off while changing connections or uploading. Disconnect the
translator's FSYNC output from **P4 GPIO1**. Connect that P4 input to the same
generator-side junction already feeding GPIO6, before the resistor divider.
Never join the generator output to the translator output. Preserve common ground.
Use short, similar-length branches; differences between those branches appear
in the result. The physical header position is not the GPIO number.

The ADC branch may remain connected through the two **220-ohm resistors** used
for the completed pulse-delay run. If it remains connected, leave ADC power on
whenever the generator is enabled. Feather may remain powered but is not used.
The new P4 firmware never commands the ADC, configures Feather UART, or accesses SD.

Keep the previously verified approximately 997-Hz, 50% square wave and generator
settings. Both P4 inputs now receive the same roughly 0-2.9 V loaded direct branch.
Do not change amplitude to compensate for the ADC resistor divider. Check that
the extra GPIO1 branch has not materially changed the direct signal levels.

## PC checks, build, upload

Karl runs these commands from `espP4`, one step at a time:

```powershell
..\test\runSmokeChecks.ps1
```

Expect 19 suites. The new analysis tests cover known signed differences, timer
wrap, a single boundary edge, missing internal pulses and malformed reports.
They do not test real P4 interrupt behavior or voltage thresholds.

Build a dedicated image and separate configuration:

```powershell
idf.py -B build/skew -D SDKCONFIG=sdkconfig.skew -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.skew.defaults" build
```

Upload to P4 only:

```powershell
idf.py -B build/skew -D SDKCONFIG=sdkconfig.skew -p COM3 flash
```

After wiring checks and with the connected ADC powered, enable the AFG output.
Close serial monitors, then run:

```powershell
python .\test\runInputSkew.py
```

The runner requires `INPUT_SKEW_P4 version=1` and opens only COM3. It performs
five 100-ms captures. `--p4` and `--runs` override port and trial count. It saves
raw timestamps, JSON statistics and `comparison.html` in `captures/input-skew/`.
The AFG is always manually controlled; turn its output off before removing power.

## Capture and analysis

GPIO1 is MCPWM group-0 capture channel 0; GPIO6 is channel 1, matching the
pulse-delay test. Both use one timer at 160 MHz (6.25 ns per tick). GPIO1 floats
and GPIO6 has the same weak pull-down used in the prior test. Both rising and
falling transitions are captured for this comparison. A rising transition means
the voltage changes from LOW to HIGH.

Each capture interrupt copies the hardware's saved edge time, not its software
execution time, into internal RAM. Two 512-event arrays bound storage; overflow
rejects the run. Core 0 stops both channels before printing either array. No
console transfer happens during collection.

The PC checks each channel for consecutive records, alternating directions and
450-550 us spacing between opposite edges. It pairs events by their times and
directions, not by blindly matching array indexes. Sequential channel enabling
can omit a boundary edge on one channel; at most one leading and one trailing
unmatched event are trimmed and reported. Internal losses and larger boundary
discrepancies fail. The 10-us pairing guard separates physical pulses; it is not
an accuracy acceptance limit. At least 100 matching events are required.

The reported difference is:

```text
GPIO1 timestamp - GPIO6 timestamp
```

Positive means GPIO1 recorded that same edge later. Negative means GPIO6 was
later. Statistics include mean, median, population standard deviation, range
and counts at each integer tick difference, separately for rising and falling.
An average between ticks does not establish sub-tick absolute accuracy.

## Interpreting a correction

A delay common to both input paths cancels in the earlier pulse-delay subtraction.
This calibration measures the difference between paths, including jumper-length
and threshold effects for this waveform. It does not isolate the chip's input
delay from those effects. Swapping matched branches in a later test can help
separate wiring effects if needed; this runner does not assume that was done.

The rising-edge result corresponds to the earlier rising input versus rising
FSYNC comparison. If GPIO1 reads 6.25 ns later, for example, that measured
relative offset would subtract 6.25 ns from the rising-path delay estimate,
subject to waveform and wiring differences. Do not automatically apply the
falling-versus-falling result to the prior falling-input measurement: FSYNC
was always timestamped on its rising edge. Saved pulse-delay results and their
PDF are not altered by this calibration.

Other uncertainties remain, including ADC response interpolation and the
startup sample-number assumption. Comparing the two pins cannot independently
validate those parts. A near-zero pin difference alone does not prove an
absolute nanosecond accuracy for the complete digitizer.

Before returning to ADC measurements, disable the AFG output, restore GPIO1's
FSYNC translator connection, and reflash the desired acquisition firmware.
Do not run the ADC delay test while GPIO1 is connected to the generator.
