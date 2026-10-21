# Short waveform capture without SD

Karl completed the PC checks, build/upload and RAM captures on October 18,
with further simultaneous captures on October 19. See [results](../docs/STATUS.md).

Reset P4 and select `w` before any SD commands. This locks RAM mode until reset;
SD prepare/test commands are rejected. Existing storage tasks allocate their
buffers but remain idle: no mount, reservation, write or readback is requested.
Default recording behavior remains available after reset without selecting w.

The standalone waveform test uses `ads127-p4-dac-60s`; simultaneous tests use
its compare/constant variants. All enable VCM for the external input.
Both boards must support this protocol: Feather now recognizes `ADC127 RAMFULL <run>` and
responds by stopping ADC and DAC normally and sending STOP. P4 then drains its
decoder and prints the waveform. Firmware predating RAMFULL gets a bounded
two-second wait followed by FAULT, not an indefinite recording. A five-second
P4 arm deadline also bounds missing/insufficient data. Feather's own 60-second
stop remains a fallback if its UART link fails.

The normal PARLIO, DMA, raw queues and decoder are retained. Core 1 discards the
first second of decoded samples for settling, then summarizes 300 ms in 600 bins
of 0.5 ms each. Each bin stores signed sample minimum, maximum and a 64-bit sum.
All samples contribute; this is not simply taking every 512th sample. At 1,024,000 samples/second, each bin contains 512 samples. Only summaries are retained, not all raw samples.
Core 0 polls an atomic completion flag and reads bins only after decoder drain.
No waveform printing or disk access happens inside the sample-processing callback.

The USB report contains WAVE_BEGIN metadata, 600 ordered WAVE rows, and WAVE_END
with the same run ID. The PC reader retains the console log and draws mean plus
extrema in a self-contained HTML file. Capture clean means no checked capture
errors and a complete window, not no distortion. The waveform is in ADC counts,
not volts. It is a shape diagnostic, not a calibrated spectrum or lossless full
sample export. USB rows are checked for count/order/run ID but have no payload CRC.
Missing DMA tails/undetected hardware loss remain the existing capture limitation.

## Karl's steps, one at a time

From repo root, PC-only collector checks:

```powershell
.\espP4\test\runRamWaveformChecks.ps1
```

Build Feather using its existing DAC environment, then upload to COM4 separately.
Build P4 in the existing IDF terminal from espP4:

```powershell
idf.py -B build/profile -D SDKCONFIG=sdkconfig.profile -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.sd.defaults;sdkconfig.profile.defaults" build
```

Then flash P4 (do not start a second monitor):

```powershell
idf.py -B build/profile -D SDKCONFIG=sdkconfig.profile -p COM3 flash
```

Profile configuration's main/control stack is increased to 6144 bytes because
the prior run showed only 388 free bytes before adding RAM report handling.
Review new stack statistics; this is provision, not a measured new margin.

Close all P4 COM3 monitors. Reset P4, then from repo root:

```powershell
python .\espP4\test\captureRamWaveform.py --port COM3
```

This reader opens COM3 and sends w; it does not command the Feather. Wait for
RAM READY, then press g in Feather's COM4 monitor. Expect VCM register 0x08=0x04,
DAC ON, RAMFULL, ADC STOP, DAC OFF, and WAVE_END. The plot path is printed under
captures/ram-waveform. Open that HTML in a browser. No SD card is required.

For another waveform, reset P4 and rerun the reader. For SD recording, reset P4
and use p instead. If the reader times out, check Feather's stop output; closing
the PC reader does not itself stop the ADC.
