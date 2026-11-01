# Grounded-input noise test

Measure the assembled ADS127L18 board with CH0P and CH0N grounded and normal SD
writing active. The test records 30 seconds through the existing acquisition,
ring buffer and SD writer. It reads the recording back and sends one second of
individual readings to the PC, starting one second after recording began.

The Python report uses all 1,024,000 readings in that second. It shows the mean
offset, standard deviation, minimum, maximum, peak-to-peak range, a histogram,
an unaveraged 2 ms waveform view, and FFT graphs covering 0–500 Hz and 1–512,000 Hz.
The time and frequency axes assume the configured 1,024,000 samples/s clock.

## What the result means

- The requested noise target is standard deviation at or below 25 microvolts.
  A perfectly fixed code fails because it cannot demonstrate working acquisition.
- The mean is reported separately; no offset correction is fitted to the data.
- FFT peaks are flagged when at least 20 dB above nearby median power. This is
  a screening aid, not a test proving randomness. Weaker tones and broad
  interference may not be flagged. Inspect the graphs as well as the numbers.
- The Hann window is applied only to the FFT, after subtracting the mean.
  Statistics use the original readings. There is no FFT averaging. Bin spacing
  is 1 Hz; the window's noise bandwidth is 1.5 Hz. ADC filtering can shape random
  noise, so a random spectrum need not be flat.
- The nominal 2.5 V reference converts signed codes into volts. The result
  includes the board's analog input circuitry and power/reference noise. The
  25 microvolt target is our system target, not a guaranteed TI limit under all
  configurations. This one-second observation does not characterize all possible
  operating conditions or the entire 30-second capture.

## PC checks first

Run these from the repository root in the Python environment used for capture:

```powershell
python -m pip install -r .\test\requirements-noise.txt
python .\espP4\test\groundedNoiseChecks.py
.\test\runSmokeChecks.ps1
```

The new Python suite checks known noise magnitudes, known 60 Hz and 100 kHz tones,
FFT scaling, signed decoding, damaged transfers, storage failures and saved graphs.
The hardware-smoke PC suite also runs the real C++ reporter on synthetic bytes
and checks its entire million-reading output in Python. These checks do not
operate the boards or establish analog performance.

## Build and upload

The Feather uses the existing `ads127-external-smoke` firmware, version 6. That
controller disables its DAC and controls ADC start/stop; it does not require a
generator to be connected. No Feather source changes are needed for this test.
If that is still the installed firmware from the Rigol tests, leave it installed.

The P4 needs the new smoke firmware, version 9. With serial monitors closed,
use the ESP-IDF PowerShell environment and enter `espP4`:

```powershell
idf.py -B build/smoke -D SDKCONFIG=sdkconfig.smoke -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.sd.defaults;sdkconfig.smoke.defaults" build
```

After the build passes, upload:

```powershell
idf.py -B build/smoke -D SDKCONFIG=sdkconfig.smoke -p COM3 flash
```

## Wire the grounded inputs

1. Switch the Rigol AFG output OFF. Remove power from the ADC board before changing
   its wiring. Disconnect the generator cable from CH0P. Disconnect any Feather
   analog-output connection and scope probes from the inputs.
2. Keep the existing CH0N short to analog ground. Short CH0P to analog ground as
   well. At each SMA input, this means connecting the center contact to that
   connector's outer shell/ground, preferably with a shorting plug or short wire
   at the board. Do not merely connect CH0P to CH0N and leave them floating.
   Avoid using long loose leads as the short; they can pick up interference.
3. Check for unintended solder bridges. Leave the digital Feather/P4 connections
   and SD card as before. Reconnect board USB connections and ADC power; reset the
   Feather and P4 before the test. Leave the signal generator disconnected.

This grounds the **board's CH0 input connectors**, not arbitrary ADC-chip pins.
The board's amplifier supplies the ADC's input common-mode voltage internally.
See the [TI ADS127L18EVM guide, sections 2.1 and 2.4](https://www.ti.com/lit/ug/sbau435/sbau435.pdf)
for the input arrangement. The scope is not needed for this measurement.

## Collect the results

From the repository root, using the Python environment with NumPy and pyserial:

```powershell
.\test\runHardwareSmoke.ps1 -Noise
```

Do not add `-External` or scope voltage readings. `-Noise` selects the DAC-disabled
controller itself and fixes the recording length at 30 seconds. It checks both
firmware versions, the serial link and P4's acknowledgement before starting.
The default ports remain Feather COM4 and P4 COM3.

After recording, the individual readings take several minutes to transfer at
115200 baud. Progress is printed in roughly 5% increments; the readback deadline
is 20 minutes. This does not extend recording time. Leave power and the SD card
connected until completion. Existing recordings are preserved; each run adds
two preallocated files of 16 seconds capacity each.

The printed capture folder contains:

- `comparison.html`: statistics, waveform, histogram and FFT graphs.
- `result.json` and `result.txt`: numeric results and checked storage evidence.
- `noise-samples.bin`: the analyzed second, exactly 3,072,000 bytes. Each signed
  24-bit reading is stored lowest byte first. Volts = signed code × 2.5 / 8,388,608.
- `feather.txt` and `p4.txt`: complete board logs, including sample-transfer CRC.

The PC requires complete ordered transfer rows, a matching transfer CRC, clean
receiver counters, no ring overflow, and the existing retained-file checksums.
A noise-target failure still saves the valid measurements and graphs. A damaged
transfer is rejected rather than analyzed as though its samples were reliable.
