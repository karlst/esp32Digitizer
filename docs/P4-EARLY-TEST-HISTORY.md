# Capture and SD test history

Historical results through October 20, 2026. Start with [current status](STATUS.md).
These are dated observations, not current wiring, upload, or operating instructions.
Build procedures live in [BUILDING.md](BUILDING.md).

## October 17: P4 capture and storage development

Status: initial revision passed Karl's PC tests and built/uploaded successfully.
Five-second bench runs were clean through 256k samples/s; 512k filled the
software queue and stopped with one dropped block. Timing-v2 also exhausted
the pool at 512k, measuring decoder throughput below the input requirement.
Grouped-v3 subsequently passed PC tests and five-second hardware runs through
1.024M samples/s. The **60-second memory-only run also completed**, receiving
61,439,431 words at an estimated 1,024,020.0 samples/s, with all reported error
counters zero. Decoder throughput was 12.792 MB/s against 8.192 MB/s required;
queue peak was 3/16. This does not establish that every generated sample arrived.
One earlier post-upload ARM received no READY; restarting P4 restored the link.
That startup handshake issue has not been diagnosed or claimed fixed.

**SD-v1 passed Karl's PC tests and built/uploaded after a callback-type fix.**
Both cards then failed mounting with FatFs error 13; both were confirmed exFAT.
SD-v2 mounted exFAT and preallocated successfully. The first combined ADC/SD
run stopped after about 7.9 seconds: 8,062,439 decoded words, zero reported
capture errors, recording-ring peak 4,194,300/4,194,304 bytes and one overflow.
It saved 32,243,708 accepted bytes; draining afterward did not recover the
rejected batch. Decoder throughput was 8.884 MB/s against 8.192 MB/s input.
SD-v3 passed Karl's PC checks and built/uploaded, but the first SD-only start
hit a stack protection fault in sdTest before its START message. The 4 KiB task
stack included a 2 KiB local batch and floating-point startup formatting.
SD-v3b moved that batch to the global object's internal RAM, reserved an 8 KiB
task stack, and used integer startup formatting. Karl built and uploaded it.
This crash produced no usable SD throughput result. Test rates, write sizes,
preallocation, pacing and the recording buffer are unchanged.
SD-only **26 Mbps passed for 60 seconds**: 195,000,000 bytes, ring peak 368,700,
zero overflow/errors/unsaved bytes, and every generated word verified.
Write-call throughput was 29.190 Mbps. **32.768 Mbps failed after 7.616 seconds**:
ring overflow, 31,194,468 accepted bytes saved, zero pattern errors, write-call
throughput 29.123 Mbps. These results concern the card currently installed;
do not assume it is the original S3 reference card.

**SD-v4 passed the combined 60-second ADC-to-SD test:** 61,439,431 samples,
184,318,293 bytes (exactly three/sample), matching readback CRC f75cf499,
zero reported capture/storage errors, overflow or unsaved bytes. Ring peak
318,413/4,194,304; max write/flush 59,943 us; write-call throughput 28.978 Mbps.
File: P4376247.BIN. This retains the capture-loss limitations described below.

**SD-v5 passed Karl's PC tests, both builds/uploads, and the 30-minute ADC-to-SD
hardware test.** At that checkpoint the installed firmware was the matched 30-minute pair.
Run 1844422070 saved 1,843,184,119 samples / 5,529,552,357 bytes to P4037876.BIN,
with matching full-file CRC32 34e6f7ed, zero reported capture/storage errors,
overflow or unsaved bytes. Ring peak was 355,717/4,194,304 bytes; maximum write/
flush 72,455 us; write-call throughput 29.029 Mbps. ADC stop was confirmed.
Earlier power-interrupted attempts failed and do not count toward this result.
DMA, packing, status checks, SD clock, ring and transfer sizes are unchanged.
Expected payload is 5,529,600,000 bytes (1,843,200,000 samples), approximately;
normal capture start/stop limitations still apply. No existing files are deleted.
The original Feather/S3 environment retains its five-second run duration.



## October 12: why capture moved from S3 to P4

- The reusable SD backend passed the 26-Mbps, 30-minute synthetic test with space
  reserved before recording. Reference commit: `f3e098a`; build environment:
  `sd-reliability-preallocated`. It uses 4 MiB PSRAM buffering, 128 KiB writes,
  and 10-MHz four-bit SDMMC. This is not an ADC-to-disk integration test.
- ADS127L18 configuration over Feather SPI works. Short S3 DMA capture tests
  passed through 256,000 samples/s. Matching status headers do not validate
  every payload bit, analog accuracy, or exact generated-sample accounting.
- S3 I2S capture failed at 512,000 and 1,024,000 samples/s (32.768-MHz DCLK).
  Its required internal clock is at least eight times DCLK: 262.144 MHz here,
  exceeding the selected 160-MHz clock. Preserve the diagnostic, stop tweaking
  delays, and replace the capture path. Full ADC output drive did not fix it.
- Ordinary default firmware still targets the legacy ADS1256/web application.
  Building it is fine; uploading it to the current ADS127 wiring is not an
  automatic restoration procedure because pin roles differ.

At that checkpoint S3 source had restored input timing bypass, but its last
reported upload still contained the failed RX_BCK_IN_DM=1 experiment. The
rollback had been built, not uploaded. Feather used ads127-capture-control.
These historical firmware states must not be treated as current installations.

PARLIO supports external-clock reception up to 40 MHz, bit packing, and continuous
circular DMA. It avoids the S3 I2S clock-ratio limit. We have NOT established
reliable ADS127L18/translator/P4 operation from specifications alone.

At 32.768 MHz, each half clock period is 15.259 ns. ADC output-valid delay is
up to 7 ns. Conservatively allowing 4.5 ns difference between translator data
and clock delays leaves about 3.759 ns setup and 8.759 ns hold, before extra
wiring/loading effects. No guaranteed P4 PARLIO pin setup/hold specification was
found. A focused hardware experiment is accepted; a guarantee is not justified.
An Espressif hardware inquiry was drafted but NOT submitted.

Use a newer ESP-IDF than the current 4.4.7 project. Older PARLIO receive drivers
had a buffer-tail corruption issue; the revised mounting code is present in
v5.5.4. Select the final version for the actual P4 chip revision. Porting the
backend adapter is separate work; retain the reusable buffered writer.

### References retained from the investigation

- [P4 PARLIO driver](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/parlio/parlio_rx.html)
- [P4 datasheet](https://documentation.espressif.com/esp32-p4_datasheet_en.html)
- [PARLIO buffer defect](https://github.com/espressif/esp-idf/issues/17581)
- [S3 I2S limitations](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/i2s.html)
- [ADS127L18 datasheet](https://www.ti.com/lit/ds/symlink/ads127L18.pdf)
- [Translator datasheet](https://www.ti.com/lit/ds/symlink/sn74axc8t245.pdf)
- [TI FPGA reference](https://github.com/TexasInstruments/precision-adc-examples/tree/main/devices/ads127L18/verilog)

### PC and build verification at that checkpoint

All twelve desktop suites passed (`espS3/test/runNativeChecks.ps1`).
Seven PlatformIO builds passed: Feather `featheresp32`, `ads127-spi-check`,
`ads127-capture-control`; S3 `esp32-s3-devkitc-1`, `ads127-input-check`,
`ads127-dma-check`, `sd-reliability-preallocated`.
No compiler warning/error matches appeared in the captured build logs.
Git whitespace checks passed. No firmware upload or hardware test was performed.
Builds and desktop tests do not establish physical capture timing.

## October 17: endurance evidence and implementation details

Karl completed the full 30-minute ADC-to-SD test and provided screenshots of
both the capture summary and final SD SAVED result. This is real ADC capture,
not the synthetic SD-only generator. The ADC was configured for its internal
short input; payload values are not a deterministic test pattern.

| Measurement | Observed result |
|---|---|
| Run ID | 1844422070 |
| Requested rate / duration | 1,024,000 samples/s for 1,800 seconds |
| Estimated received rate | 1,024,020.3 samples/s |
| Complete received samples | 1,843,184,119 |
| Saved payload | 5,529,552,357 bytes, exactly 3 per received sample |
| File | `P4037876.BIN` on the card |
| Full-payload readback CRC32 | `34e6f7ed`, matched submitted payload |
| Capture bad headers / period errors / short words / drops / errors | All zero |
| Ring peak | 355,717 of 4,194,304 bytes (about 8.5%) |
| Maximum streaming write/flush duration | 72,455 microseconds |
| Ring overflow / storage errors / unsaved bytes | All zero |
| Time-in-write-call throughput | 29.029 Mbps; not whole-pipeline throughput |
| ADC stop | Echo confirmed; ADC status after stop 0x00 |
| Final verdict | SD SAVED |

The endurance binary was subsequently deleted intentionally by Karl; it is not
retained in this repository. The values above come from his console records.

### Hardware recorded at that checkpoint

- P4: Waveshare ESP32-P4-Module-DEV-KIT, original board, silicon revision 1.3.
  Configured for pre-v3 silicon, CPU 360 MHz, 16 MB flash, DIO/40 MHz flash;
  module has 32 MB PSRAM, enabled at 200 MHz for the recording ring.
- P4 USB UART: CH343, COM3. Feather ESP32 V2 USB UART: CH9102, COM4.
  Port assignments were confirmed by their actual console banners, not assumed.
- Feather controls ADS127L18 configuration/start/stop through its existing SPI
  connection. P4 receives sample signals directly from ADC through translator.
  Feather does not generate the recorded samples or supply a DAC test signal.
- ADC external clock 32.768 MHz. DCLK at highest test rate is also 32.768 MHz.
- Translator SN74AXC8T245: VCCA/VDDA from Feather 3.3 V; VCCB/VDDB from ADC
  approximately 1.8 V. ADC assembly powered separately from 6 V DC.
- All grounds common, continuity checked. Old shared 5 V jumper between boards
  removed; each MCU board uses its own USB power/data cable. Do not reconnect
  their 5 V or 3.3 V rails. The DAC coax was disconnected for that internal-short experiment.
- Do not leave one linked MCU intentionally powered while the other is off.

| Signal | P4 connection | Other end |
|---|---|---|
| DCLK | GPIO0 input | Translator A5; formerly S3 GPIO9 |
| FSYNC | GPIO1 input | Translator A6; formerly S3 GPIO10 |
| DOUT0 | GPIO2 input | Translator A7; formerly S3 GPIO11 |
| Command RX | GPIO4 | Feather TX / GPIO8 |
| Command TX | GPIO5 | Feather RX / GPIO7 |
| Console UART | GPIO37/38 | Reserved for P4 USB UART |
| SD CLK / CMD | GPIO43 / GPIO44 | Onboard microSD socket underneath board |
| SD D0/D1/D2/D3 | GPIO39/40/41/42 | Onboard four-bit SDMMC |

SD IO voltage domain uses internal LDO4. The warning `voltage value 0 is out of
the recommended range [500,2700]` originates when IDF creates its adjustable LDO
driver with an unset initial voltage, before setting operating voltage. It is
not a voltmeter reading and did not prevent mounting or the successful recording.

Two cards were tried and both were confirmed exFAT. Exact identity of the card
installed then was not established; do not call it the original S3 card.
The documented S3 reference card was SanDisk Extreme 64 GB V30 A2.

### Capture and recording architecture established then

1. PARLIO uses the incoming ADC clock, capturing DOUT0 and FSYNC simultaneously
   on rising DCLK edges. FSYNC is captured as data, not used to gate reception.
2. At full rate, 32 clocks x 2 captured bits = 64 bits = 8 DMA bytes per ADC sample.
   Raw DMA throughput is 8.192 MB/s. This is distinct from the CPU's 360 MHz clock.
3. Cyclic DMA uses a 32 KiB internal buffer. CPU0 ISR copies each completed
   descriptor into one of 16 private buffers, up to 4096 bytes each. Queue
   ownership prevents CPU1 decoding memory already overwritten by DMA.
4. CPU1 grouped decoder checks frame edges/periods and ADC status, obtains an
   8-bit status plus 24-bit sample word, and emits batches up to 512 words.
   Optimized lookup/group processing was needed to sustain full-rate capture.
5. Before ring submission, pack only the low 24 sample bits as three little-endian
   bytes. Status is still checked/counted; it is not saved with each sample.
   SD demand is 3.072 MB/s = 24.576 Mbps, not the old 32.768 Mbps four-byte format.
6. The reusable `shared/bufferedWriter` owns a 4 MiB PSRAM ring. Submission
   copies without disk waits. CPU0 disk task writes 128 KiB blocks through a
   16 KiB internal-RAM transfer buffer, using four-bit SDMMC at 20 MHz.
7. `p` prepares a unique create-new file and reserves exactly 10,000,000,000
   bytes before READY in that endurance build. The later smoke build reserves 128 MiB. Reservation requires exFAT and contiguous allocation.
   No formatting, overwriting or deletion. Insufficient space rejects preparation.
8. Feather ARM/READY handshake precedes ADC START. ADC STOP is followed by DMA
   shutdown/decoder drain and prompt DONE acknowledgment. Disk finalization is
   separate: drain ring, trim unused reservation, sync, read back full payload,
   compare CRC, update valid header, sync and close. Only then SD SAVED/FAILED.

Source locations of these sources (after the October 20 cleanup):
`espP4/main/src/` contains main, decoder and recorder implementations;
`espP4/main/include/` contains their interfaces and ADC recording format.
SD-only generation and data checks are in `espP4/test/hardware/`.
The generic bufferedWriter remains reusable and independent of the ADC.

Raw DMA and decoded-byte totals now uint64 because 30 minutes moves about
14.746 GB before decoding. File totals/timing are uint64. Valid per-sample/header
counts remain uint32, bounded at this duration with compile-time rate margin.
Ring head/tail intentionally stay atomic uint32: unsigned differences and a
power-of-two ring correctly wrap at 4 GiB. PC tests exercise that actual wrap
without creating a large disk file. This does not authorize unlimited duration.

### Filesystem and format decisions

ESP-IDF's bundled FatFs has exFAT code disabled by default. Initial mounts failed
with error 13 (FR_NO_FILESYSTEM). `espP4/cmake/exfat.cmake` copies SDK FatFs
into the selected build directory and selects it as the overriding component.
SDK installation is unchanged. Copy enables FF_FS_EXFAT=1, FF_USE_LFN=3 and
FF_MAX_LFN=255. It also defines disabled FF_USE_LABEL numerically as 0 to avoid
an exFAT compilation error. Component/header consistency is checked; override
is pinned to IDF 5.5.6 and FatFs config revision 80286. Review before SDK upgrades.

ADC v3 header is 512 bytes, magic `P4ADC03` plus zero at offset 0:

| Offset | Type | Meaning |
|---|---|---|
| 8 | uint32 LE | Version 3 |
| 12 | uint32 LE | Valid 0/1 |
| 16 | uint32 LE | Run ID |
| 20 | uint32 LE | Rate-table stage |
| 24 | uint32 LE | Requested samples/second |
| 28 | uint64 LE | Payload byte count |
| 36 | uint32 LE | Payload CRC32 |
| 40 | uint32 LE | Bytes/sample = 3 |
| 44 | uint64 LE | Stored sample count |
| 52–511 | zero | Reserved |

Payload starts at 512: signed 24-bit little-endian samples. FF FF FF = -1;
00 00 80 = -8388608. Sign-extend bit 23 when reading. Old P4ADC01 has four-byte
records. P4ADC02 has three-byte records but a different 32-bit size header.
Synthetic P4SYN01 remains version 1, four-byte generated words, special stage
0xffffffff. Do not conflate these formats or silently reinterpret older files.

Initial header is invalid until final verification. Close failures can follow
header writing, so console SD SAVED remains required. Power-interrupted files
can retain a 10 GB reservation. No automatic cleanup was performed. Failed
captures that finalized normally were trimmed and preserved with invalid headers.

### Failures and fixes worth retaining

- After some uploads the Feather got no READY/DONE although P4 console worked.
  Resetting P4, preparing a new file, then retrying restored the handshake.
  Cause remains undiagnosed. Do not describe it as fixed. Failed READY means
  Feather did not start the ADC; do not wait 30 minutes for a test never started.
- A small SD-test stack crashed before START. Fixed by moving its 2 KiB batch
  into static object memory, increasing task stack to 8 KiB, and replacing
  floating-point startup formatting with integer formatting.
- heap_caps_free required a correctly typed uint8_t* wrapper for writer ownership.
- Multiple endurance attempts lost power (apparently affecting ADC while MCUs
  could still report). They had bad ADC headers and SD FAILED, despite no write
  overflow/errors. These are not endurance passes. One run reached roughly
  4.07 GB; a stop command was unconfirmed. Reinitialization preceded the final run.
- Final successful run 1844422070 is separate from failed IDs 2253990025 and
  4142401717. The new successful result supersedes pending-validation notes.
- The original S3 I2S capture path hit a peripheral-clock limitation at high rates;
  CPU speed alone would not fix that path. P4 PARLIO plus optimized decoding
  solved reception. Both chips already had DMA and four-bit SD capabilities.
- SD-only 26 Mbps passed for 60 s (195 MB, every word checked), but 32.768 Mbps
  overflowed at ~7.6 s. Packing three sample bytes solved the combined-run
  storage demand. Do not return to four-byte ADC records without a new reason.
- Original S3 synthetic preallocated 26 Mbps/30-minute result remains useful
  history, but it was not ADC-to-SD and may have used a different physical card.

## October 18–19: analog signal and measurement investigation

| Experiment | Evidence / result | What it establishes |
|---|---|---|
| CardP 30-minute SD run, 1844422070 | 1,843,184,119 samples; 5,529,552,357 payload bytes; CRC 34e6f7ed; zero reported capture/storage errors | Sustained operation with this card/run, not every card or guaranteed absence of sample loss |
| CPU profile, 3506626047 | Core 0 busy 40.3%, Core 1 92.6%; 60 valid intervals; SD SAVED | Decoder core has limited measured headroom; includes profiling overhead |
| CardP sine recording, 3866327264 | 61,439,999 samples; CRC 46f24c94; reported errors zero; flattened waveform | Storage/capture diagnostics passed; sine fidelity did not |
| RAM waveform, 468923391 | Distortion remained after enabling VCM; reported capture faults zero | SD is not required to reproduce the distortion; VCM change did not fix it |
| Feather A1-to-A2 loopback | TI disconnected: 752..1899 raw counts, sine-shaped. Connected: 1593..1900, lower region flattened | Connection changes the waveform at the source; precise electrical cause unresolved |
| Simultaneous single-reading comparison, 3173507255 | Held-out RMS difference 62.039 Feather counts, 11.772% of input swing | Shape comparison only; gain/offset/time were fitted |
| Simultaneous burst comparison, 2268306345 | RMS difference 16.980 counts, 4.670%; clean reports | Averaging reduced disagreement; does not justify production averaging |
| Exact digital PC test | Karl reported PASS across 160 cases | Actual decoder and three-byte packer preserve tested values; no physical PARLIO/DMA execution |
| Constant DAC, 561890385 | Feather RMS 17.392 counts; clean reports | Jaggedness remains without a changing command |
| Calibrated constant DAC, 1931182800 | Feather RMS 16.071 mV, peak-to-peak 91.828 mV; ADS127/P4 RMS 0.153 mV, peak-to-peak 1.325 mV; clean reports | Feather averaged measurement fluctuates far more on the shared scale; different filters/averaging still apply |

Karl reported successful portable profiler, DAC math, analog-reader, RAM waveform,
comparison and constant-report checks during implementation. The latest constant
build and upload completed before the 07:32 capture on October 19. After the
readability cleanup, Karl reran comparisonChecks.py and constantChecks.py and
reported both passed on October 19. These were PC checks, not new hardware runs.

### Retained evidence

`docs/evidence/` contains unchanged copies of Feather/P4 text logs for
20261018-201209, 20261018-202101, 20261019-070141 and 20261019-073229,
plus the disconnected/connected loopback text records under `dac-loopback/`.
Original logs and selected HTML reports are preserved in `docs/evidence/`. Temporary CSV exports and build products are not
needed in the commit; the report scripts and text logs retain the useful inputs.
Older endurance/profile numbers above come from Karl's supplied console records
and the October 17 endurance record above, not newly rerun tests.
Karl intentionally deleted old CardP files; the endurance binary is not retained.

### Interpretation at that checkpoint

The blue jagged-line investigation is closed for now: evidence strongly points
to the Feather measurement as the main contributor, not corrupt P4 decoding.
This does not uniquely distinguish source noise, converter noise and filtering.
The Feather reported factory reference calibration (source=0); voltage conversion
uses SDK table interpolation. ADS127 conversion assumes a 2.5 V reference and
unity board gain. Neither is a precision calibration of the whole measurement chain.

DAC lower-waveform flattening with TI connected remains unresolved. Loading is a
leading hypothesis, not a confirmed fault. Karl is considering a standalone signal
generator; no purchase or replacement-source capture has been confirmed.

Card F overflowed during a short capture while CardP passed. Identical branding
does not qualify both cards; underlying cause remains unproven. Whole missing
frames and the final DMA tail can escape current capture diagnostics. ADC data
CRC and a physical known-sequence test remain proposals, not implemented features.

Normal SD output remains unaveraged signed 24-bit values in three little-endian
bytes/sample. Production error policy, card qualification and migration of
Feather duties are still open. Current firmware is a bench checkpoint.

## October 20: hardware smoke test

The test runs the Feather DAC, ADS127, P4 DMA/decoder, and SD writer for 30 seconds,
then compares A2 measurements with actual SD readback. All reported receive and
storage counters were clean in the runs discussed below; full payload checksums
matched. The DAC alternates two levels once per second.

- `20261020-093826`: maximum individual-bin disagreement was 44.0 mV (17.8% of
  input swing), failing the original 5% limit. Average low/high values agreed
  within about 2–5 mV. Short A2 bursts fluctuated much more than disk averages.
- `20261020-095403`: after switching to separate settled averages for each
  plateau, the worst difference was 20.8 mV (8.2%). This failed 5%, then passed
  when reanalyzed under Karl's approved 10% functional-smoke criterion. That
  reanalysis was not a new hardware run. The original failure log remains intact.
- `20261020-100816`: a new run passed at 6.8%, with 30,719,711 samples,
  92,159,133 payload bytes, and 29.9997 seconds. The worst plateau difference
  was 17.05 mV. All 29 expected level changes were detected. File: P464e4b7.BIN.

The smoke build now reserves 128 MiB; longer recording builds retain 10 GB.
One HTML step plot overlays measured A2 input and SD output. Its vertical edges
mark commanded one-second boundaries, not measured transition shapes. No fitted
gain or offset is applied. The 10% threshold is for gross functional failures,
not precision qualification. Build/upload/run instructions and limits are in
[HARDWARE-SMOKE.md](HARDWARE-SMOKE.md).

October 20 raw captures and plots remain in the local ignored `captures/` folder;
the numerical summary here survives a source checkout, but those files do not.
