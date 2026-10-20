# Constant-input fluctuation diagnostic

Authorized October 19, 2026 to investigate jagged Feather A2 readings versus
smooth ADS127/P4 readings. Karl runs checks, builds, uploads and hardware steps.
Karl completed checks, build/upload and captures, including calibrated reporting
in run 1931182800. See [results](../docs/STATUS.md).

Separate Feather environment `ads127-p4-constant` holds DAC code 135, nominal
1.747 V, near the previous sine peak and above the observed flattened region.
It repeats the same write every 500 us to preserve the earlier scheduling load.
Normal sine and production builds are unchanged. P4 firmware is unchanged.
Keep A1->A2 and the existing TI coax/ground wiring. The reader still selects
RAM mode, waits for both boards, then starts one bounded stage-6 capture.

First run `python .\espP4\test\constantChecks.py` from the repository root.
Then build/upload the Feather environment above using PlatformIO. Close serial
monitors and reset P4 before running:

```powershell
python .\espP4\test\captureComparison.py --feather COM4 --p4 COM3 --constant
```

The reader requires explicit DAC mode identity before starting. Older comparison
firmware without that identity must be rebuilt before using this revised reader.
Raw logs and constant.html are saved under captures/waveform-comparison.

There is no fitted scaling, time alignment or pass threshold. The first two plots
share millivolt limits when calibration metadata is present; the later raw plots
retain independent count axes. Dark curves retain the previous roughly 0.5 ms means;
pale envelopes show individual-reading minima/maxima already collected by the
firmware. Summary RMS is fluctuation around each trace's own mean, not error
between instruments. Unequal reading counts and hardware filters remain; this
test cannot uniquely attribute noise to source versus ADC or qualify production.

## Shared voltage scale (October 19 follow-up)

Updated Feather firmware exports INPUT_CAL and CAL table rows after acquisition
stops. The SDK uses factory two-point/reference data when available, otherwise
the explicitly labelled nominal 1100 mV fallback. No calibration work or extra
conversions occur in the measurement loop. A new capture is required: earlier
raw-only logs cannot reveal which calibration this particular Feather has.

The report now begins with two plots using identical, symmetric mV bounds.
Each trace has its own mean subtracted, with no gain fit or time shift. Feather
voltages interpolate the SDK's integer-mV lookup table at 32-count spacing;
converting burst means is approximate in nonlinear regions. ADS127 uses the
assumed 2.5 V reference and unity board input gain. Original count plots and raw
extrema remain below. Different ADC filtering and averaging still apply.

Karl runs constantChecks.py again, rebuilds/uploads ads127-p4-constant on COM4,
then resets P4 and repeats the same --constant capture command. P4 firmware and
wiring remain unchanged. This sequence was completed on October 19; use it to repeat the test.
