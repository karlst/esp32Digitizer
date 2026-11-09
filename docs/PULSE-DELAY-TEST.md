# Direct-pulse / ADC response delay

This RAM-only test sends the same generator transition to ADC CH0 and a spare
P4 digital input. MCPWM hardware timestamps the direct input and FSYNC using the
same 160-MHz timer. It reuses the version-3 startup-discard and association checks.
It neither writes SD nor changes the production recording format.

## Build and test

Karl runs all commands. First, from `espP4`:

```powershell
..\test\runSmokeChecks.ps1
```

There are now 19 PC suites. The added analysis tests use known 2.5-us transitions
at different sample phases, timer wrap, missing edges, wrong pulse numbers,
flat ADC input and report generation. They do not establish circuit timing.

Build in a separate directory and configuration:

```powershell
idf.py -B build/delay -D SDKCONFIG=sdkconfig.delay -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.sd.defaults;sdkconfig.delay.defaults" build
```

Then upload only P4:

```powershell
idf.py -B build/delay -D SDKCONFIG=sdkconfig.delay -p COM3 flash
```

The image identifies as `PULSE_DELAY_P4 version=2`. Keep Feather's external-source
version-6 image; it leaves its DAC disabled. The association-only runner refuses
this image, and the pulse-delay runner refuses the association-only image.

## Wiring used for the November 5 measurements

P4 GPIO6 is exposed on the carrier header. The checked carrier schematic routes
this net between the module and header; it is separate from GPIO0/1/2 capture,
GPIO4/5 Feather UART and GPIO37/38 console. Use the printed **GPIO6** label,
not physical connector pin 6. GPIO6 stays an input with a weak pull-down.

The tested arrangement uses two equal 220-ohm resistors. The ADC loads the
midpoint: measured levels were 1.46 V high and 128 mV low at CH0P, and 2.9 V
high and -11 mV low on the direct branch. Do not assume an exact half-voltage
ratio once the ADC is connected.

```text
Rigol signal, 0 to 3.3 V -----+-------------------- P4 GPIO6
                            |
                           220 ohm
                            |
                            +-------------------- ADC CH0P (0 to ~1.65 V)
                            |
                           220 ohm
                            |
Common ground --------------+-------------------- ADC CH0N
                            +-------------------- P4 GND
                            +-------------------- Rigol return
```

Keep the existing ADC clock, FSYNC, data and Feather control connections.
Perform rewiring with generator output off and board supplies disconnected.
Keep leads short, with nearby ground returns. The 3.3-V branch must not be wired
directly to CH0P. Do not join the boards' power rails.

Set the generator to a square wave, **997 Hz, 50% duty**, nominal low 0 V and high
3.3 V, with its load setting appropriate for a high-impedance circuit. Using
997 Hz instead of an exact submultiple of the sample clock helps vary the
transition's position between ADC readings. This is a measurement stimulus,
not a GPS/PPS simulation.

Before connecting the P4 signal lead, verify the actual loaded waveform with
the scope: the direct branch should be about 0–3.3 V, without overshoot above
the P4 supply; the CH0 branch should be about 0–1.65 V, below the 2.5-V input
range. Power the ADC before enabling the generator. Turn generator output off
again before connecting the P4 signal lead or removing board power. Requested
generator settings alone do not establish these voltages.

## Capture

After wiring and voltage checks, power both boards and ADC, enable the verified
generator waveform and close serial monitors. From `espP4`:

```powershell
python .\test\runPulseDelay.py
```

For the 10-kHz comparison, restore GPIO1 to translated FSYNC after the input-skew
test, build/upload the version-2 delay image, and set the generator manually to
**10,000 Hz, 50% duty**, keeping the verified amplitude and offset. Keep output
off during rewiring/uploads. Reset both boards before starting a fresh series.
With the ADC powered and the verified signal enabled, run:

```powershell
python .\test\runPulseDelay.py --frequency 10000
```

The frequency argument checks the expected pulse spacing; it does not control
the generator. Both JSON and HTML record the requested and measured frequency.
Only 997, 1000 and 10000 Hz are supported. The existing 40-sample window fits
between 10-kHz transitions (50 us apart); plateau checks remain unchanged.

Defaults are P4 COM3, Feather COM4, five trials. No generator setting is changed
by the script. Each trial retains roughly 32 ms of ADC data, stops acquisition,
then exports forty individual raw words around each bracketed direct edge.
Logs, JSON and a standalone `comparison.html` go to `captures/pulse-delay/`.
The generator remains under manual control; turn output off before ADC power.

## How the measurement works

The existing FSYNC channel has no interrupt for each ADC sample. A second
capture channel on the same timer records both edges of GPIO6. Its interrupt
copies the already captured hardware timestamp into a bounded 4096-event array.
The time the interrupt executes is not used as the pulse time. No printing or
dynamic allocation occurs in the callback. Overflow rejects the trial.

Core 0 stops direct capture before inspecting that array. After Core 1 has
finished decoding, only edges between the first and last retained FSYNC anchors
are eligible for export. At most the first 128 consecutive eligible edges are
exported, bounding serial transfer time; the captured count includes all edges,
including startup and those beyond the export limit. The known startup discard
maps pulse 2 to stored word 0. The PC
rechecks this mapping and uses the measured ticks-per-sample ratio to place each
ADC reading on the direct pulse's time scale. It refuses missing/repeated edges,
bad headers, unsteady plateaus and absent or incomplete analog steps. The
accepted spacing is within 10% of half the requested period: 45–55 us for
10 kHz, or approximately 450–550 us for 1 kHz. Missing internal edges still fail.

Two measures are saved:

- **Response center:** the average time of successive voltage changes, weighted
  by their signed sizes. This estimates the center of the filtered response,
  including ringing. For a linear system with a settled response it estimates
  the low-frequency group delay; it is not a frequency-by-frequency calibration.
- **Half-height:** the time the response reaches halfway between its measured
  plateaus, interpolated between adjacent ADC readings. Saved separately because
  half-height and group delay need not coincide for an asymmetric response.

The report separates rising and falling response-center medians, standard
deviations and ranges. It also plots individual transitions aligned to their
direct timestamps. No averaging of sample bins hides the transition shape.

## Limits

This measures the **analog-input-to-FSYNC path relative to the direct GPIO path**.
It includes the board's analog input circuitry, ADC conversion/filter behavior,
and existing FSYNC translator propagation. The divider, wiring and GPIO threshold
add uncalibrated differences. No typical datasheet delay is silently subtracted.
If the spare translator replaces the divider, its propagation belongs in the
analog branch and must also be accounted for.

The timer resolution is 6.25 ns; it is not a complete accuracy bound. ADC samples
are approximately 976.6 ns apart. The response-center and half-height calculations
estimate between readings, and their discretization/model error is not bounded
by the scatter of repeated trials. A low standard deviation alone does not
justify applying a nanosecond-accurate correction. Review the measured response,
sample-phase dependence and branch delays before using a fixed correction.

FSYNC association retains its documented startup and undelivered-tail
limitations. A small spacing residual cannot independently rule out a constant
whole-sample offset in all associations. This test provides measured evidence,
not an independent absolute calibration of that assumption.

## References

- Local [carrier schematic](reference/waveshare-esp32-p4-module-dev-kit-schematic.pdf)
  and [header pinout](reference/waveshare-esp32-p4-module-dev-kit-pinout.webp).
- [Espressif P4 electrical specifications](https://documentation.espressif.com/esp32-p4_datasheet_en.html).
- ESP-IDF 5.5.6 `esp_driver_mcpwm` capture callback: `cap_value` is latched by
  hardware; `cap_edge` identifies the direction. Both channels use one timer.
