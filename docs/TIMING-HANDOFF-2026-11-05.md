# Timing work: November 5 handoff

Karl stopped for the day after approving a fixed timing correction. Resume with
adding timing information to recorded samples. Do not restart the delay debate
or repeat completed measurements without a concrete new reason.

## Agreed decision

Use **2553 ns** as the **measured input-to-FSYNC delay, including the voltage
translator**, for the tested configuration. Keep it configurable.

```text
Estimated sample input time = corresponding FSYNC time - 2553 ns
```

This is an operational correction supported by the measurements below, not an
ADC-chip-only filter group-delay specification or a fully quantified absolute
accuracy guarantee. Karl explicitly accepted moving forward with this limitation.
The rising-response median was 2556.335 ns; subtracting the measured rising-input
difference of 3.044 ns gives 2553.291 ns, rounded to the agreed 2553 ns.
No production timing correction or sample timestamp records have been implemented
by this decision alone.

## Completed evidence

### FSYNC-to-word association

- Five trials passed: `captures/fsync-association/20261105-132153/`.
- Firmware identity: `FSYNC_P4 version=3`.
- Known stopped-ADC startup, hardware pulse counter and FSYNC timer capture.
- The first captured ADC frame has 31 clocks on this hardware. Diagnostic mode
  explicitly discards the first frame, whether 31 or 32 clocks; hardware pulse
  **2** maps to retained word **0**. Other startup lengths and later framing
  errors remain failures. Ordinary recording retains its previous behavior.
- Earlier version 2's zero-anchor problem was fixed with short direct-register
  snapshots. Version 3 fixed the startup discard and pulse-to-word accounting.
- Timestamp spacing agreed to less than one 6.25-ns timer tick. This is
  consistency evidence, not an independent absolute sample-number calibration.
- No evidence of a one-sample mistake was found. The initial mapping remains a
  checked-startup assumption; do not describe a 977-ns error as observed or as a
  quantified uncertainty. That earlier explanation unnecessarily confused Karl.

### ADC path-delay measurement

- Five completed trials: `captures/pulse-delay/20261105-153442/`.
- 354 transitions: 176 rising, 178 falling.
- Rising median **2556.335 ns**, population standard deviation **9.064 ns**,
  range **2533.300-2579.737 ns**.
- Falling median **2554.701 ns**, population standard deviation **7.572 ns**,
  range **2535.991-2579.520 ns**.
- Maximum FSYNC spacing residual across these trials: **5.487 ns**.
- Same generator edge went directly to P4 GPIO6 and through a resistor divider
  to ADC CH0P. P4 GPIO1 received FSYNC after its existing voltage translator.
- Timer: MCPWM group 0, 160 MHz, 6.25 ns/tick. Raw ADC windows contain 40 readings
  per transition. Reported center is the signed voltage-change-weighted time;
  half-height interpolation is saved separately. No waveform-bin averaging.
- Graph normalizes each low plateau to 0% and high plateau to 100%. Falling
  transitions are inverted so both directions overlay on the same curve.
- PDF saved and visually checked, two identical copies:
  `docs/test-results/2026-11-05-adc-pulse-delay.pdf` and
  `output/pdf/2026-11-05-adc-pulse-delay.pdf`.
  It intentionally documents the delay measurement before input calibration.
- Raw data, graph and logs remain in the capture directory. Do not overwrite
  this evidence or silently revise its raw measurements with the correction.

### P4 input comparison

- Five completed trials: `captures/input-skew/20261105-161802/`.
- Firmware identity: `INPUT_SKEW_P4 version=1`.
- Same generator signal directly into GPIO1 and GPIO6, translator FSYNC wire
  disconnected from GPIO1. No ADC acquisition, Feather commands or SD access.
- 1003 matched edges, no boundary trims: 501 rising, 502 falling.
- Difference sign is **GPIO1 timestamp minus GPIO6 timestamp**.
- Rising mean **+3.043912 ns**: 257 pairs at 0 ticks, 244 at +1 tick.
- Falling mean **+4.121016 ns**: 171 pairs at 0 ticks, 331 at +1 tick.
- Every individual difference was 0 or +6.25 ns. Fractional-tick means are
  averages, not proof of fractional-tick absolute accuracy.
- Includes relative jumper-path and input-threshold effects. Common input
  latency cancels. Rising-to-rising result is relevant to the earlier rising
  input versus rising FSYNC comparison. Do not blindly subtract the falling
  calibration from falling-input results: FSYNC was always captured on its rise.
- JSON, raw timestamps and `comparison.html` are saved. A separate PDF of this
  input comparison has not been requested or generated.

## Current bench state: restore before ADC acquisition

- **P4 currently runs the input-skew calibration image**, built in `build/skew`
  with `sdkconfig.skew`, uploaded on **COM3**.
- **GPIO1 is currently connected to the generator**, not ADC FSYNC. GPIO6 is
  also on the same direct generator signal. Do not run ADC acquisition this way.
- To restore ADC work: AFG output off first; remove the generator branch from
  GPIO1 and restore its translator FSYNC connection; load the appropriate P4
  firmware. Do not tie generator output to translator output.
- GPIO0 = translated DCLK; GPIO1 normally = translated FSYNC; GPIO2 = translated
  DOUT0; GPIO4/5 = Feather UART receive/transmit. GPIO numbers are board labels,
  not physical header positions.
- Feather **COM4**, external-source bench firmware version 6, DAC disabled.
- ADC supply was on and generator enabled for the final test; shutdown after
  that has not been confirmed. AFG output must be off before removing ADC power.
- Generator setting: square, **997 Hz**, 50% duty, nominal 3.3 Vpp and +1.65 V
  offset, High-Z setting. User measured the actual loaded levels below.
- Final divider uses **two 220-ohm resistors**, not 2 kohm. Generator-side node
  feeds direct P4 inputs; midpoint feeds CH0P; bottom resistor and CH0N go to
  common ground. CH0P has no grounding jumper.
- Loaded levels reported by Karl: direct branch **2.9 V / -11 mV**, CH0P
  **1.46 V / +128 mV**, both clean square waves. Do not assume exactly half
  voltage: the ADC board's amplifier input loads the divider.
- Earlier 2-kohm divider troubleshooting included a probe attenuation mismatch.
  Scope probe and scope channel must match 10x. Karl used CH2 for these readings.

## Firmware/software and test status

- Latest complete PC smoke run: **19/19 passed**, confirmed by Karl.
- Karl built, uploaded and ran all three hardware diagnostics successfully.
- Dedicated configs: `sdkconfig.fsync.defaults`, `sdkconfig.delay.defaults`,
  `sdkconfig.skew.defaults`. Do not replace production startup with test objects.
- Runners: `runFsyncAssociation.py`, `runPulseDelay.py`, `runInputSkew.py` under
  `espP4/test`. The last opens only P4 COM3.
- Serial link initially timed out during pulse-delay work. Link-only checks
  passed with AFG both off and on. Exact original transient cause was not proved.
- The shared FSYNC/pulse runner now checks the serial link once before trial 1.
  Repeating `@LINK` after acquisition was a runner bug: P4's destination-lock
  flag rejects rearming diagnostics even while idle. Fixed without reflashing.
  Reset both boards before a fresh full FSYNC/pulse series if needed. The
  input-skew runner has no such Feather handshake or destination lock.
- Windows Smart App Control blocked locally built PC test executables. Karl
  switched it off and tests then passed. Do not change Windows security settings.

## Resume direction

Earlier discussion favored a single SD stream of repeated **[Timing block]
[Sample block]** groups, with the following sample count in the timing block,
and UTC origin in the file header rather than redundantly in every timing block.
Review the exact record layout and existing storage implementation before coding;
this is a design direction, not a completed format specification.

Tie a directly captured GPS/PPS edge to the ADC sample timeline using FSYNC,
then apply the configurable 2553-ns correction when assigning input times.
Current calibration tests do not yet timestamp real GPS or write timing metadata
to the recording files. GPS hardware previously produced roughly 3.3-V,
100-ms-wide PPS pulses after satellite acquisition; current bench source is Rigol.

Karl wants short, concrete explanations and one manageable build/upload/wiring
step at a time. He runs PowerShell, builds, uploads and hardware tests himself.
Read-only inspection and explicitly authorized edits may proceed. Discuss new
implementation scope and obtain Go when needed; do not ask twice for already
authorized actions. Karl handles all Git operations; none were performed here.
## 10-kHz follow-up completed

Karl restored GPIO1 to translated FSYNC and authorized the 10-kHz comparison.
The delay image is now version 2: 4096 direct-edge slots, at most 128 consecutive
eligible waveform exports. The runner accepts `--frequency 10000`, checks pulse
spacing, and saves requested/measured frequency. Synthetic regression coverage
includes 10-kHz timing, bounded export and missing-edge rejection. Karl passed
19/19 PC suites, built/uploaded the image and completed five hardware trials.
Saved session: `captures/pulse-delay/20261105-170932/`.
The displayed rising medians were 2551.0, 2557.2, 2556.3, 2552.2 and 2550.0 ns;
falling medians were 2548.8, 2556.7, 2555.4, 2550.6 and 2552.9 ns.
These are raw GPIO6-relative measurements, before the separate input-skew
correction. The agreed operational correction remains 2553 ns.

## Documentation cleanup, November 6

Karl authorized a documentation-only cleanup before committing the timing tests.
The FSYNC, pulse-delay and input-skew firmware and PC analysis now explain
execution order, ownership, units, sample mapping, counter wrap, calculations
and failure handling. Shared serial reading, Feather external-source hooks,
synthetic timing checks and PC-check scripts also have expanded explanations.
Start at `espP4/test/SOURCE-LAYOUT.md` for the reading paths. No measurement
algorithm, protocol, firmware setting or saved result was changed by this cleanup.

