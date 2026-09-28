# ESP32 Digitizer – Old Firmware Reference

## Purpose

This document preserves what was recovered from the ESP32-S3 before replacing its existing firmware. It is intended as a historical reference for the new `s3Recorder` project, not as the design specification for the new implementation.

## Firmware Backup

Original full-flash backup:

```text
backups/s3OriginalFlash.bin
```

Flash size:

```text
16 MB (16,777,216 bytes)
```

SHA-256:

```text
C3DFDA0497E81D5433047055DCADB506805F2F679A17C7654C001057A3223F99
```

The backup was read from the ESP32-S3 over its USB serial/programming connection before any new firmware was uploaded.

The ESP32-S3 reported:

```text
Chip: ESP32-S3 (QFN56), revision v0.2
PSRAM: 8 MB embedded
Flash: 16 MB
Crystal: 40 MHz
```

## Recovered Firmware Identification

Readable strings extracted from the original flash strongly identify the old application as an ADS1256 test/demo program.

Recovered banner:

```text
ADS1256 - Custom Library Demo File by Curious Scientist - 2025-05-28
```

Other recovered application strings include:

```text
DRATE:
STATUS
Opening data.txt for writing
/data.txt
Finished Write
Total conversion time for 150k samples:
Sampling rate:
The serial connection is OK!
Single-ended conversion result:
PGA value:
DRATE is selected as:
DRATE is set to
Value of
 register is:
```

The flash also contained SD-card related strings:

```text
/sdcard
Card Mount Failed
No SD_MMC card attached
SD_MMC Card Type:
SD_MMC Card Size: %lluMB
```

This indicates that the old firmware was not merely a stock ADC example: it had been extended to exercise SD storage as well.

Additional embedded build/version strings included:

```text
12:15:52
Jul 22 2025
v5.5-1-gb66b5448e0
arduino-lib-builder
```

These are retained only as historical clues; they do not by themselves define the new project's toolchain.

## Recovered ADS1256-to-ESP32-S3 Wiring

The following mapping was reconstructed from the existing physical wiring and the ADS1256 module header labeling.

| ADS1256 Signal | Wire Color | ESP32-S3 Connection |
|---|---|---:|
| PDWN / SYNC | tan | GPIO46 |
| DRDY | green / teal | GPIO9 |
| CS | yellow | GPIO10 |
| DIN / MOSI | purple | GPIO11 |
| SCLK | gray | GPIO12 |
| DOUT / MISO | blue | GPIO13 |
| GND | black | GND |
| 5V | white | 5V |

### Notes on confidence

The ADS1256-side signal order and the S3-side GPIO locations were reconstructed from close-up photographs of the still-connected hardware.

The mapping is internally consistent:

- GPIO11 = MOSI
- GPIO12 = SCLK
- GPIO13 = MISO
- GPIO9 = DRDY
- GPIO10 = CS
- GPIO46 = PDWN / SYNC

This matches the physical wire placement and provides a sensible SPI/control grouping.

Because this is a reconstruction from the surviving wiring rather than recovered C++ source, retain the original photographs and do not treat this document as proof that every line was used identically by every historical firmware build.

## Old Firmware Behavior Worth Remembering

The old firmware appears to have done at least the following:

1. Initialized the ADS1256.
2. Selected and reported ADS1256 DRATE settings.
3. Reported PGA configuration.
4. Performed single-ended ADC conversions.
5. Timed a run of 150,000 samples and reported the effective sampling rate.
6. Mounted an SD card using SD/MMC support.
7. Opened `/data.txt` and wrote data to it.

This is useful evidence that the same ESP32-S3 and ADS1256 hardware had already been exercised together before the current `s3Recorder` project.

## Reuse Guidance for the New Project

For initial ADS1256 bring-up, start with the recovered physical wiring above rather than inventing a new pin assignment.

However, do not copy the old firmware architecture blindly. The new project has a different target architecture:

```text
ADS1256 -> ESP32-S3 -> ring buffer -> microSD
```

with a target of:

```text
30,000 samples/second
one channel
continuous recording
```

The old firmware is most valuable as a hardware and bring-up reference.

Recommended reuse:

- Preserve the old flash image permanently.
- Preserve the SHA-256 above.
- Keep the current ADS1256 wiring unchanged during initial bring-up.
- Use the Curious Scientist ADS1256 library/example as a reference for register setup and basic communications.
- Re-test all assumptions under the new PlatformIO project.
- Measure actual sustained sample rate rather than assuming the old demo achieved the new project's requirements.
- Build the new ring-buffer and SD-writing architecture independently.

## Suggested Repository Location

```text
esp32Digitizer/
    backups/
        s3OriginalFlash.bin
        s3Strings.txt
    docs/
        oldFirmwareReference.md
    s3Recorder/
    featherUi/
```

## Historical Warning

Do not delete `s3OriginalFlash.bin` after the new firmware is working. It is the only complete preserved image of the pre-existing S3 firmware and can be restored or re-examined later if needed.
