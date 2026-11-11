# Delay calibration: bench handoff

Updated November 10, 2026, at the end of the probe comparison.

## Where we stopped

Karl is revisiting the 2553-ns timing correction to establish a defensible
uncertainty. Do not describe repeatability or the 6.25-ns capture tick as an
absolute accuracy bound. The original calibration did not record the actual
analog input rise time or GPIO6 switching threshold.

Today both scope probes were connected to the same Rigol AFG signal to check
relative probe/channel delay. Rigol DHO914S CH1 and CH2 traces visually coincided.
The automatic Delay(r-r) measurement reported <1 fs (femtosecond), effectively
zero calculated difference. Record the finding as **no measurable relative delay
in this setup**, not zero physical delay or a femtosecond accuracy bound. We have
not established a numerical uncertainty bound from this check.

## Last bench settings

- Both probe tips on the same AFG signal, grounds at the same ground.
- CH1 and CH2: 10x probe settings, DC coupling, 1 V/div.
- AFG: square wave, 1 kHz, 3.3-V amplitude, +1.65-V offset, 50% duty.
- Last supplied image: 20 ns/div, 625 MS/s, 2 ns/point. The 2 ns/point label is
  sample spacing, not the horizontal division setting.
- Waveform showed overshoot and ringing; both traces overlapped.
- Automatic measurement: Measure -> Other -> tap Delay(r-r); sources CH1/CH2.
  Tapping the measurement tile adds it to the Result sidebar; no separate Add.

The earlier probe-compensation signal had a slow rise. Use the AFG signal for
this comparison. Confirm actual settings when resuming; no hardware was operated
by the assistant.

## Next step, when Karl resumes

Walk through one manageable wiring/measurement step at a time. Use the two probes
to examine the actual calibration input at GPIO6 (direct branch) and CH0P (loaded
resistor-divider branch). Measure the rise shape and relative timing under the
actual calibration load. This will help quantify input-path differences; it
will not by itself measure GPIO6's exact digital switching threshold.

Then agree on how to measure or bound that threshold and combine systematic
uncertainty with repeatability before claiming a 95% interval for the correction.
Do not launch another build, upload, or calibration run without Karl's direction.

## Original calibration essentials

The Rigol signal went directly to GPIO6 and through a divider to ADC CH0P.
Translated FSYNC went to GPIO1, and translated DOUT0 supplied digital sample
values. The P4 hardware captured edge timestamps on a common timer. The plotted
voltages came from ADC sample codes; they were not oscilloscope measurements of
the analog input edge or of the translator's logic voltage.

The reported delay used a voltage-change-weighted transition center, not linear
interpolation of a single 50% crossing. Raw rising centers were near 2556 ns;
a measured GPIO1-minus-GPIO6 rising skew of about 3.04 ns led to the rounded
2553-ns correction. This includes the tested signal path, not just the ADC chip.

Saved evidence:

- Original 997-Hz calibration: captures/pulse-delay/20261105-153442/.
- Additional 10-kHz calibration (not 100 kHz): captures/pulse-delay/20261105-170932/.
- GPIO input-skew check: captures/input-skew/20261105-161802/.
- Curvature analysis: output/diagrams/calibration-rise-curvature.svg and companion
  HTML/JSON, if retained locally.

The original 176 rising-edge centers had median 2556.335 ns, standard deviation
9.064 ns, and range 2533.300 to 2579.737 ns. These describe repeatability, not an
absolute uncertainty interval. Input edge shape/threshold bias remains unresolved.

## Separate completed work to retain

UTC-named files passed the five-minute test in
captures/fsync-association/20261110-081808/, run 943984754, card directory
0:/TIMED/S000013: five files, 299 complete intervals, 306174894 samples,
918540554 bytes, zero errors/unsaved bytes, full readback verification. Peak ring
use was 2264319 bytes; maximum write was 526643 us. UTC was synthetic.
STATUS.md still describes filename verification as pending; update that stale
section in a future documentation cleanup. No Git operations were performed.
