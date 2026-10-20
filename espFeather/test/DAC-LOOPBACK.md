# Independent measurement of the bench DAC

Karl completed this diagnostic October 18, both disconnected and connected to
the TI input. See [results](../../docs/STATUS-2026-10-19-P4.md). Use bench-dac-loopback, not the old web
application. The exact existing benchDac/benchDacWave source generates the sine.
No ADS127 register operations, UART commands, P4 capture or SD access occur.

## First comparison: TI input disconnected

With ADC 6 V off and board USB power disconnected, disconnect the coax from the
TI board and jumper Feather A1 (GPIO25, DAC1) to Feather A2 (GPIO34, ADC1 channel
6). A1 and A2 labels are board pin names, not adjacent GPIO numbers. Keep the
loose coax connector from contacting anything. Power only Feather for this
first measurement. No extra ground jumper is required between pins on the same
board. Do not connect A2 to the TI input or VCOM instead of A1.

The goal is to compare requested sine codes with actual Feather output measured
by its own ADC. This removes the TI input load, but A2 itself is still a load.
The Feather ADC has noise and nonlinearity; this test identifies gross distortion,
not precision amplitude, distortion or calibration. A clean result would point
toward the connection/load or downstream TI/decoding path. A distorted result
would keep the Feather output and this measurement setup under investigation.
Do not conclude the DAC is defective from a single unverified loopback reading.

## PC and firmware steps (Karl runs these)

Build from repository root; no board connection is required:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run --project-dir .\espFeather -e bench-dac-loopback
```

After wiring and connecting Feather USB, close COM4 monitors and upload with
the same command plus `-t upload --upload-port COM4`. Firmware leaves DAC off
at boot. Then, with no other COM4 monitor open:

```powershell
python .\espFeather\test\captureDacLoopback.py --port COM4
```

The reader sends g. The DAC runs for about 1.3 seconds: one second settling,
then 300 ms of measurements. A2 readings occur nominally every millisecond in
Arduino loop, independently of the DAC timer's 500 us updates. Each reading
has its actual conversion midpoint timestamp. Missed measurement slots are
reported separately from missed DAC updates; they are never filled with invented
samples. All results remain in bounded global RAM until DAC is disabled, then
USB output starts. Driver faults or manual s abort and mark the result unclean.

The reader saves the full serial log and an HTML plot under captures/dac-loopback.
Plot axes are elapsed milliseconds and raw Feather ADC counts (0..4095), not
ADS127 counts. Clean means the collection completed with no known driver errors
and sufficient readings, not that the signal is a sine. Review DAC diagnostics
and plot together. The textual export checks row count/time ordering but has no
CRC. The board stops without the reader; reader cleanup also sends s.

To resume P4 analog testing later, restore ads127-p4-dac-60s firmware and the
agreed wiring with power off. Do not send P4 ARM commands from this build: it
does not implement the ADC controller. A later loaded-output comparison should
be planned separately after viewing this first plot.
