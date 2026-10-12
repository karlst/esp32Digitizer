# ADS127L18 bench tests

## Five-second sample capture

This test checks whether the S3 can receive samples from the new ADC. It does
not write to the SD card or run the web page. Keep the DAC coax disconnected:
the ADC selects an internal zero-difference test input instead.

The Feather program is `ads127-capture-control`; the S3 program is
`ads127-dma-check`. Neither starts the ADC automatically. With both programs
installed and the verified power/translator connections restored, USB stays on
the Feather. At 115200 baud, send one command for one five-second test:

| Command | Samples/second | ADC data clock | 32-bit slots per frame |
|---|---:|---:|---:|
| `g` | 16,000 (passed baseline) | 4.096 MHz | 8 |
| `1` | 32,000 | 4.096 MHz | 4 |
| `2` | 64,000 | 4.096 MHz | 2 |
| `3` | 128,000 | 8.192 MHz | 2 |
| `4` | 256,000 | 16.384 MHz | 2 |
| `5` | 512,000 | 32.768 MHz | 2 |
| `6` | 1,024,000 | 32.768 MHz | 1 |

Current results (October 12, 2026): 16k through 256k passed short board tests
with valid headers and no reported overruns/faults. Both 512k and 1024k failed
with bad headers at 32.768 MHz DCLK. At 1024k, 2529 of the first 4096 headers
were bad (2340 were 0x10 and 189 were 0x11); only 1567 were valid. These are
received-word checks, not proof that every payload bit or generated sample arrived.

The S3 I2S slave receiver requires an internal clock at least eight times DCLK.
Our 160-MHz source cannot meet 32.768 MHz * 8 = 262.144 MHz. The high-rate
stages remain available as historical diagnostics, not supported recording modes.
Do not resume input-delay experiments to work around this documented limit.
See [the project handoff](../../docs/STATUS-2026-10-12.md) for the replacement plan.

Changing ADC output drive from half to full strength did not solve reception:
full-drive 512k had 3596 bad headers in 4096 words. Full drive remains in source
because it matches TI's timing specification; it does not change the 1.8-V level.

The diagnostic saves a count for each header byte, the first eight bad words
with zero-based received positions, and the receiver configuration/timing/clock
registers. These appear as `ADC127 REGS`, `HEADERS`, `BADWORD`, and `DIAGEND`
lines through Feather USB after `DONE`. No long report is printed during capture.
Counts reset each run. A shifted header suggests alignment trouble but does not
by itself identify the cause or establish the validity of the sample bits.

The `RX_BCK_IN_DM=1` clock-delay experiment failed at 512k: all 4096 headers
were bad (00=1562, 01=1, 10=668, 18=1863, 19=1, 40=1). Register readback
confirmed `timing=10000000`. The source now restores bypass (`00000000`) on every
run. This experiment did not establish the root cause; no 1024k run was made
with delay enabled. Do not shift received words in software to hide bad headers.

The 256k stage uses OSR64, ADC clock divided by two for DCLK, and two-slot framing.
The receiver reads up to four DMA blocks per poll so its one-ms task yield does
not impose a one-block-per-ms processing limit. USB status output is buffered.

At 512k/1024k, sixteen 4000-byte DMA blocks hold the received data; the CPU reads
up to 4096 words per loop. The 1024k stage needs a one-slot hardware override
because IDF's public API clamps frame length to at least two slots. This override
is applied with reception stopped and checked before READY. Correct hardware
readback alone does not establish that one-slot capture works: inspect measured
rate and headers. If either high-rate stage fails, stop and diagnose it.

Only the first slot enters DMA. `s` stops early; `h` prints help. Test each rate
in order and review its counts/status before sending the next command. Nothing
automatically advances to a higher rate. Any observed bad header, DMA error,
overrun or event-queue pressure stops the S3 stage and asks Feather to stop ADC.
If no samples arrive, the five-second timer still stops the test; zero data is
not a pass. Counts should be close to the requested rate times five seconds,
allowing for startup and partial final blocks. Stop advancing on unexpected
counts, fault flags, or any nonzero error counter.

Both boards need the new firmware: ARM2/READY2 messages include and verify the
selected stage. Older firmware cannot acknowledge these requests and therefore
cannot accidentally start a mismatched ADC/receiver configuration.

The sequence is:

1. Feather logs ADC status, clears only the startup-history flags, and checks
   status again. Other faults or flags that remain prevent starting. It then
   writes ADC settings and reads them back.
2. S3 prepares its DMA buffers and replies READY.
3. Feather starts the ADC at the selected rate.
4. S3 reports received data once per second through Feather USB.
5. Feather stops the ADC after five seconds, logs status without clearing it,
   then asks S3 to stop.

Look for roughly the selected number of words/second during full reporting intervals,
increasing `words` and `valid`, and zero `bad`, `overruns`, and `errors`.
The first and last intervals can be shorter. Final counts omit any unfinished
DMA block and are not an exact measurement of every sample the ADC generated.
`eventQueueFull` warns that error notifications themselves might have been lost.
Nonzero bad headers mean the incoming word format or ADC status needs inspection.

For this test each DMA word holds an eight-bit ADC status header followed by
one signed 24-bit sample. The header helps check sample boundaries. It is not
the proposed recording format. Passing this low-rate test does not establish
operation at one million samples/second.

The first hardware run failed: about 50,000 received words/second, no valid
headers, and zero latest words in each report. Register readback succeeded,
which confirmed the requested values arrived, not that those values were right.
Two software corrections followed: CLK_CFG=0x08 selects the external clock
(0x00 selected the internal clock), and the S3 explicitly applies and checks
the eight-slot frame layout because the installed IDF 4.4 driver ignores the
initial total_chan field. The corrected five-second run received 79,872 words,
with approximately 16,000 words/second and no reported DMA overruns. Reported
latest words had header 0xC8 and small varying sample values. That header includes
the ADC error flag, which startup history also asserts. The startup-clear/status
checks described above were added afterward; subsequent short runs through 256k
passed the header checks. This paragraph records the earlier commissioning run.

Reading path: `hardware/ads127CaptureControl.cpp` coordinates the test;
`../src/adc/ads127LowRate.cpp` explains each ADC setting;
`../../s3Recorder/test/hardware/ads127CaptureDiagnostic.cpp` handles S3 commands;
`../../s3Recorder/src/frontend/ads127Dma.cpp` contains the actual `i2s_read()` call
that copies completed DMA data for inspection.

## First communication check

This standalone diagnostic replaces the web application only when explicitly
built/uploaded as `ads127-spi-check`. Ordinary `featheresp32` remains the default.
It disables both DAC outputs, starts no web server or S3 commands, and sends no
SPI traffic at boot. USB diagnostics use 115200 baud. Send `h` for help or `r`
for one finite, three-pass read test after the electrical checks pass.

Build only from the repository root (neither command uploads):

```powershell
pio run -d featherUi -e ads127-spi-check
pio run -d s3Recorder -e ads127-input-check
```

## Reading path

1. `../../shared/ads127Pins.h`: agreed physical GPIO assignments.
2. `hardware/ads127SpiDiagnostic.cpp`: manual command, raw replies and status explanation.
3. `../src/adc/ads127l18.cpp`: actual register-read transfers.
4. `../../s3Recorder/test/hardware/ads127InputDiagnostic.cpp`: S3 input-only entry point.

This diagnostic sends read-only two-byte SPI frames at 100 kHz, mode 1. It accounts
for the ADC returning the requested register in the NEXT frame. It assumes the
power-on default, with SPI CRC disabled. It never writes settings, clears flags,
resets the ADC or requests conversions. Start is physically held low.

Expected identity: DEV_ID 0x06 for ADS127L18. Revision varies. Status 0x60 is the
documented reset value: its analog-low and reset-history flags are both initially
set. The diagnostic does not clear them, so it cannot establish whether a later
voltage fault occurred. Identity match and status interpretation are reported
separately. All-00/all-FF or inconsistent identification is a failed check, not
enough information to identify which connection is wrong.

## Before a later, separately authorized upload

For this already voltage-checked setup, turn off the ADC supply, unplug USB,
and remove the OE-to-GND jumper before uploading. Keep the coax disconnected.
The digital signal wires can remain installed: removing OE disables the
translator outputs. Reconnect USB to the board being uploaded. Existing power
wiring may power both ESP32 boards from one USB cable.

After both programs are installed, unplug USB again before restoring the OE
jumper. Restore board power using the checked 6 V ADC supply, 1.8 V VCCB and
3.3 V VCCA arrangement. Keep MODE/RESET high and START grounded. Do not change
wiring with power on. The diagnostic cannot
sense ADC power; sending `r` is an operator action after those checks, not an
automatic firmware power-interlock. With this S3 diagnostic the Feather's normal
web UI would report S3 disconnected because no status protocol is running.

The three-pass SPI check returned stable DEV_ID=06, REV_ID=01, STATUS=60 on
2026-10-11. That verifies the register communication path, not DMA capture.
The five-second tests above were performed afterward; see their current results.

Protocol reference: [TI ADS127L1x datasheet Rev B](https://www.ti.com/lit/ds/symlink/ads127l18.pdf),
sections 7.5.2–7.5.4 and 8.1–8.3. The PC test is
`../../s3Recorder/test/ads127DriverChecks.cpp`, included in its native test runner.
