# Thirty-second hardware smoke test

This checks Feather DAC -> ADS127 analog input -> P4 -> SD card -> readback.
The boards perform the capture and readback; a PC script coordinates them,
compares the reports, saves detailed logs, and prints a short PASS/FAIL result.
It uses the existing real ADC driver, decoder, recorder, and disk checksum.
Test-specific code is under the board `test` folders and compiled only when selected.

## Before building

Run the PC smoke checks from the repository root:

```powershell
.\test\runSmokeChecks.ps1
```

There are now eleven suites. The two additional suites exercise the real readback
parser, square-wave timing, and rejection of missing, corrupt, or mismatched reports.
Passing these checks does not establish hardware performance.

## Build and upload Feather

In ordinary PowerShell, from the repository root:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run --project-dir .\espFeather -e ads127-hardware-smoke
```

After a successful build, close any COM4 monitor and upload:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run --project-dir .\espFeather -e ads127-hardware-smoke -t upload --upload-port COM4
```

The default Feather environment is unchanged; this is an explicitly selected test.

## Build and upload P4

Use the [ESP-IDF PowerShell activation](BUILDING.md), then:

```powershell
Set-Location C:\Users\karls\source\esp32Digitizer\espP4
idf.py -B build/smoke -D SDKCONFIG=sdkconfig.smoke -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.sd.defaults;sdkconfig.smoke.defaults" build
```

This creates a separate smoke configuration and build directory. After a successful
build, close any COM3 monitor and upload:

```powershell
idf.py -B build/smoke -D SDKCONFIG=sdkconfig.smoke -p COM3 flash
```

Neither board starts the signal or recording at boot.

## Connect and run

Use the existing DAC-to-ADC coax, A1-to-A2 jumper, ADC power and inter-board wiring.
The ADC input is CH0P, with CH0N grounded. A2 must observe the same signal wire.
Both boards need USB; the known assignments are Feather COM4 and P4 COM3.
Insert the exFAT SD card. The smoke firmware reserves 128 MiB (134,217,728 bytes)
before capture, covering even the 35-second timeout with a 10% rate margin.
Sufficient contiguous free space is required. The final file is shortened to
about 92.16 MB of sample data plus its 512-byte header. Longer recording builds
retain the 10-GB reservation. Existing files are not deleted or overwritten.
Use this image for the 30-second smoke test; its smaller reservation is not sized
for the separate 60-second SD-only speed commands.

Close serial monitors and reset both boards before each run. From the repository root:

```powershell
.\test\runHardwareSmoke.ps1
```

The script first checks that both boards identify as the correct idle firmware.
It prepares SD, starts stage 6 (1,024,000 samples/second), records for 30 seconds,
then waits for full readback. Preparation and readback add time beyond 30 seconds.
Alternate ports can be supplied as `-Feather COM4 -P4 COM3`. Python needs pyserial,
as do the existing capture scripts. The runner never builds or uploads firmware.

Typical output (illustrative values):

```text
Digitizer -> P4 DMA: OK - no reported receive errors
Recording buffer: no overflow
SD written: 92,160,000 sample bytes
SD readback: checksum matches
DAC signal measured at A2: OK - two voltage levels and 29 changes detected
Hardware smoke test: 30 seconds
Recorded: 30.000 seconds
Signal: 1.480 / 1.750 V; 29 level changes verified
Largest plateau-average error: 2.1% of signal swing (limit: 10%)
PASS
```

Check confirmations print as the completed reports are validated, before the
voltage verdict. They are retained even if that final comparison fails. The byte
count excludes the 512-byte file header. These are confirmed post-capture results,
not live claims about hardware still running.

A failure gives a plain-language reason and a nonzero process result. Complete
board logs and the result are retained under `captures/hardware-smoke/<timestamp>`.
No card transfer to the PC is needed. If interrupted, the runner requests stop;
Feather also has a 30-second stop and P4 a 35-second timeout. Keep the card installed
until writing/readback has finished. USB failure is not confirmation of a clean stop.

## What is checked

- The DAC alternates codes 110 and 135, one second at each level, starting low.
  These are commands, not assumed physical voltages. Feather measures the actual
  A2 voltage with a short 500-us burst every 100 ms throughout the run.
- P4 records every decoded sample as three bytes. Its existing CRC checksum
  checks every payload byte read from SD against the submitted data. Acquisition
  faults, overflow, write errors, final sync failure and close failure reject the run.
- During that same disk readback, test code reconstructs signed samples across
  read-block boundaries and summarizes 300 consecutive 100-ms bins. It does not
  substitute a second live RAM capture for the data on disk.
- All 29 expected level changes must appear in both reports, within one 100-ms
  bin of their expected positions. A stuck or reversed signal cannot pass.
- Comparison uses six settled bins per second, from 0.2 to 0.8 seconds after
  each change. For each plateau, average its six disk bins and separately average its six
  A2 burst measurements. Compare those two voltages. Each of the 30 plateaus
  must pass independently; errors in different seconds cannot cancel. The
  largest plateau-average voltage difference must be at most 10% of the
  measured input swing (high minus low). For a 300-mV swing this is 30 mV.
- Input swing must be at least 150 mV. The fixed 10% limit is a provisional
  functional threshold, not a precision specification. It is not relaxed
  automatically after a failure.

## Measurement limits

Feather uses its SDK count-to-millivolt calibration table. A missing factory
calibration produces an explicit nominal-calibration note. ADS127 conversion
assumes the existing 2.5-V reference and unity input gain. There is no fitted
voltage gain, offset, or phase shift to make a recording pass. An inaccurate
reference or either ADC's calibration can cause a legitimate test failure.

The two ADCs have independent clocks and different averaging/filtering. Settling
margins make this a steady-level functional comparison, not a precision edge or
noise test. The recording duration may differ by up to 20 ms to accommodate the
receiver's final incomplete DMA block. Reported receive errors always fail, but
this tolerance and the hardware counters cannot prove that every ADC-generated
sample arrived. CRC also is not proof against every possible corruption.

Readback occurs on the mounted card, without a power cycle. PASS establishes
this one 30-second capture, not endurance, production qualification, or all SD cards.

## One input/output plot

Each complete, valid capture saves `comparison.html` in its capture folder, even
if the voltage comparison fails. It overlays the blue A2 input and orange SD
readback on one voltage scale over 30 seconds. Each horizontal step is one settled plateau
average; vertical edges mark commanded one-second boundaries, not measured
transition timing or shape. No gain or
offset is fitted. Missing or malformed evidence does not produce a plot.
