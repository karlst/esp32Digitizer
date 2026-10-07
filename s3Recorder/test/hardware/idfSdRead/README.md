# Alternative-driver SD read test

This independent PlatformIO project builds the existing
`../sdCardDiagnostic.cpp` with ESP-IDF instead of Arduino's bundled IDF.
The ordinary recorder project keeps its original framework and configuration.
The test reads existing sectors only; it never writes, formats, or erases the card.

Build with `platformio run -d s3Recorder/test/hardware/idfSdRead` from the repository
root; add `-t upload --upload-port COM4` only when the S3 is connected and an upload
is authorized. Startup prints the actual ESP-IDF version. USB serial is 115200.
The comparison runs once at boot; sending `R` repeats it. Feather UART is absent.

`src/Arduino.h` is a small serial/delay compatibility interface, not the Arduino
framework. Its implementation uses UART0 for USB-bridge logging and FreeRTOS for
delays. CMake compiles the original diagnostic source directly, so both tests use
the same sectors, pins, bus widths, clocks, comparison logic and buffers.

The default configuration requests 240-MHz CPU operation and an 8-KiB main task
stack. Framework defaults can still differ, so a changed result would implicate
the SDK/runtime combination rather than prove a particular SD driver fix.

Card wiring: 3V to S3 3.3V, common GND, CLK5, CMD7, D0=6, D1=15, DAT2=16,
D3/CS=4. Remove test LEDs and the resistor test lead before running. Invalid-CRC
buffers are diagnostic clues only: the SDK does not guarantee valid or complete
bytes following an error return.


## Verified result, 2026-10-06

Built and uploaded on COM4; boot confirmed ESP-IDF 5.5.0 and 240-MHz CPU.
One-bit at 4 MHz read all four sectors correctly before and after the four-bit
attempts. Four-bit at 400 kHz, 4 MHz and 20 MHz failed initialization while reading
the SD Status Register (`sdmmc_init_sd_ssr`, invalid CRC / 0x109). Therefore no
four-bit sector comparisons ran under this SDK. This differs from 4.4.7, where
initialization succeeded but sector 32768 failed CRC. It is not a fix and does
not establish a specific hardware cause. Log: `../../../.pio/sdio-idf55.log`.

When the S3 runs this diagnostic, it stops after completion. Send R to
repeat; Feather communication is intentionally absent. Restore the normal
recorder by building/uploading its original PlatformIO project, not this one.

Initial installation required the ESP-IDF 5.5 toolchain and supporting tools;
these are now cached. The CMake component list includes PlatformIO's __pio_env
helper. After changing component selection, clean this project's generated build
files before rebuilding to prevent stale linker inputs.


### Direct-jumper follow-up

With the same firmware/card/GPIO assignments and the replacement breakout,
bypassing the breadboard using six-inch direct jumpers made all four sectors
pass exact comparison in four-bit mode at 400 kHz, 4 MHz and 20 MHz. The one-bit
baseline and final repeat also passed. No CRC or timeout errors occurred in this
run. Log: `../../../.pio/sdio-idf55-direct-jumpers.log`. This implicates the prior
physical interconnect arrangement, without distinguishing contact resistance,
wire geometry or ground return. It is a short read test, not write verification
or a sustained-throughput result. No card contents were changed.

The subsequent original-SDK recorder test still failed selected metadata reads
at 20 MHz. The shared diagnostic was expanded from four to six sectors to cover
those failures. Original-SDK six-sector comparisons passed at 400 kHz and 4 MHz,
but failed the added sectors at 20 MHz. Original-SDK full file verification and short acquisition both passed
at 4 MHz. Do not treat the earlier four-sector pass as full-card qualification.

Expanded newer-SDK test (2026-10-06): the first run and 17 additional repeats
passed all six sectors at every speed. Repeat 18 failed CRC on sector 65654 at
20 MHz; its 512 payload bytes matched the reference, but the controller still
rejected the transfer. Matching payload does not override a CRC failure or
identify its cause. All other phases in that repeat passed. Logs are under
`../../../.pio/sdio-direct-expanded-idf55*.log` (local, ignored).

Immediately returning to the old SDK without rewiring reproduced CRC failures
at 20 MHz on sectors 65536, 65654 and 66048; its slower phases and final one-bit
phase passed. Log: `../../../.pio/sdio-direct-expanded-idf44-recheck.log`.
The newer stack performed better in this comparison but did not eliminate the
fault. Neither a specific hardware cause nor a driver-only cause is established.
No card writes were performed in this driver comparison. Subsequent one-bit
versus four-bit write tests and current installed firmware are documented in
the root README; the board is no longer running this read-only diagnostic.
