# Feather DAC to ADS127 channel 0: 60-second functional test

Karl completed the PC checks, build/upload and real signal recording on October 18.
The recorded waveform was flattened; capture/storage checks passed on CardP.
See [results and current status](../docs/STATUS-2026-10-19-P4.md).

## Signal and wiring

Karl confirmed the TI ADS127L18EVM with CH0P/CH0N SMA connections ready for use,
in response to the channel-0/grounding question. For this single-ended test:

- Feather A1 / GPIO25 / DAC channel 1 -> coax center -> EVM **CH0P**.
- Coax shield -> common ground; **CH0N must be grounded** for this arrangement.
  Use the established negative-input grounding connection, not an arbitrary
  jumper inferred from a connector number. No connection goes to VCOM or the
  1.8 V digital translator. Do not put a 50-ohm terminator across the DAC output.
- Keep existing SPI, sample-data, UART and common-ground wiring intact. Make the
  analog connection with supplies off; ADC 6 V off first, then board USB power.
  Restore both board USB supplies and then ADC power before commanding a run.

The EVM's THS4551 analog front end accepts the signal through CH0P/CH0N and drives
the ADC differentially. This is not a direct wire to bare ADC input pins.
[TI EVM guide, section 2.1](https://www.ti.com/lit/ug/sbau435/sbau435.pdf)
describes single-ended inputs with CHxN grounded and the unity-gain front end.

The source requests a nominal **10 Hz sine**, **1.25 V offset**, **0.5 V peak**
(1 V peak-to-peak; 0.75..1.75 V). At nominal 3.3 V DAC supply this gives codes
58..135. Supply error, DAC nonlinearity and output loading affect actual volts.
Updates occur nominally every 500 us, 200 updates per cycle. Task scheduling
introduces jitter; missing update intervals are reported, not concealed. This
coarse 8-bit source verifies function, not ADC precision or distortion limits.

## Firmware isolation and sequence

New Feather environment: `ads127-p4-dac-60s`. Existing internal-short test and web
builds remain separate. P4 keeps the already uploaded `sd-profile-60s` firmware.

The analog build passes `externalInput=true` to `ads127LowRate::configure()`.
CH0_CFG1 at 0x11 changes from 0x20 (internal short) to 0x00 (external normal
polarity). GEN_CFG1 at 0x08 now changes from 0x00 to 0x04 for external input,
enabling the ADC midpoint-voltage output connected to the EVM amplifiers.
All writes are still echoed/read back. The filter, sample rate,
channel power states and status+sample serial format remain unchanged.
[TI ADC datasheet table 8-20](https://www.ti.com/lit/ds/symlink/ads127l18.pdf)
defines these settings.

October 18 follow-up: CardP run 3866327264 saved P4c7fe62.BIN with CRC 46f24c94,
61,439,999 samples and no reported capture/storage errors. The DAC reported no
missed intervals or driver faults. File analysis found a 10 Hz response, but
plotting showed severe lower-cycle flattening, not a faithful sine wave.
That run used GEN_CFG1=0x00. Karl authorized enabling VCM as a single-setting
comparison. Karl built/uploaded it; the RAM capture remained distorted. VCM enable
was not a fix for the observed flattening. The amplifier has its own
midpoint bias when its control input is open; connected-board behavior and DAC
loading still need verification. Keep the waveform, wiring and P4 firmware the
same for the comparison. Expect `ADC register 0x08: wanted 0x04, read 0x04, OK`.

Boot leaves DAC disabled and ADC idle. With P4 prepared, Feather `6` configures
the ADC and asks P4 to arm. Only matching READY starts the DAC, then ADC START.
A DAC initialization/start failure refuses the test. Stop/timeout/receiver-fault
paths request ADC stop and disable DAC output. Callback DAC driver errors also
request stop. DAC OFF means disabled/undriven, not a measured zero voltage.

The DAC reports `updates`, `missedIntervals`, `maxGapUs` and `driverFault` after
stop. A P4 SD SAVED alone cannot establish successful DAC generation: check the
Feather report too. A source fault can cause an early STOP yet still yield a
valid shorter P4 file. A received sample with saturation alone also does not
increase the P4's bad-header count under the current diagnostic policy.

## PC checks and firmware build

From the repository root in ordinary PowerShell:

```powershell
.\espFeather\test\runBenchDacChecks.ps1
```

This checks the actual phase-to-DAC-code helper without hardware. Separately,
with Python available, check the saved-file reader:

```powershell
python .\espP4\test\analogReaderChecks.py
```

Then build Feather only:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run --project-dir C:\Users\karls\source\esp32Digitizer\espFeather -e ads127-p4-dac-60s
```

After successful build, close the Feather monitor and upload with the same
command plus `-t upload --upload-port COM4`. Confirm the actual port if Windows
renumbers it. Reopen the monitor and type `h`: expect ADS127 DAC TEST, external
CH0, 10 Hz, 60 seconds, and 65-second receiver deadline. The banner describes
configured behavior, not measured wiring or voltage. No P4 rebuild is needed.

## Run and interpretation

After the powered wiring is ready: P4 `p` -> SD READY -> Feather `6` -> READY2,
DAC ON and ADC START echo -> automatic stop after 60s -> DAC OFF -> P4 PROFILE
and final SD SAVED. Do not remove card/power before finalization. Retain the
capture, header histogram, PROFILE, DAC and SD output. Repeated startup-handshake
failure remains a separate known issue; it is not proof of analog failure.

Require zero bad/period/short/drop/error counts, zero storage overflow/errors/
unsaved bytes and zero DAC driver faults. Review all header values, particularly
MOD_FLAG. Compare CPU usage/buffer peaks with the internal-short profile.

After safe shutdown, copy the new BIN to the PC without deleting earlier files.
The file header does not record external-input selection or waveform settings;
retain console run ID/file name alongside this test description.

```powershell
python .\espP4\test\inspectAnalogRecording.py 'C:\path\to\copied-recording.BIN'
```

The reader verifies file structure, valid flag and full payload CRC, then
examines 1..3 seconds (skipping the initial transient). It reports min/max/mean,
AC RMS in raw counts and a crossing-based approximate frequency. It does not
claim calibrated volts, distortion performance, no clipping, or a whole-record
waveform check. Review amplitude/offset/clipping and, if needed, plot selected
samples before calling the analog path verified. A CRC match proves saved data
matches submitted bytes, not that the physical input was correct.
