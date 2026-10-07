# Recording backend: start here

This is the S3 card adapter and background storage service. The actual ring
buffer and byte writer are in `../../lib/bufferedWriter/src`. That library has
no ADC, Feather, UART, file-format or SD pin dependencies.

## Reading order

1. `recordingConfig.h`: reservation policy, card pins and disk task settings.
2. `recordingService.h`, then `recordingService.cpp`: the application-facing
   lifecycle, background task, and published status.
3. `../../lib/bufferedWriter/src/bufferedWriter.cpp`: `submit()` copies bytes,
   `pump()` writes them, and `finish()` drains the queue and synchronizes storage.
4. `sdRecordingSink.cpp`: numbered files, reservation, headers and finalization.
5. `sdmmcBlockDevice.cpp`: actual card transfers through `sdmmc_write_sectors()`.

## Connecting the future DMA reader

DMA means hardware copies incoming data into memory. The future reader task will
take completed DMA blocks, pack samples if necessary, then call the backend.
It must not submit PSRAM data from a cache-disabled interrupt.

| Caller | Call | Meaning |
|---|---|---|
| Startup | `begin()` | Create the disk task; card readiness is reported separately. |
| Acquisition task, stopped | `prepare(rate)` | Wait for new file/reservation; true means ready for submissions. |
| One acquisition task | `submit(data, byteCount)` | Copy bytes into the ring without disk I/O or waiting for space. |
| Acquisition task | `failed()` | Check the shared fault flag without querying the card. |
| Acquisition task, producer stopped | `finish(complete)` | Wait for queued data to drain and the file to close. |
| Communications task | `snapshot()` | Copy published status; it may lag a blocked write. |

After successful submission, the input block can immediately be reused. A false
return means stop production and call `finish(false)`; do not retry or silently
skip a block. Overflow and short writes remain failures even if earlier bytes
are subsequently saved. A flush failure cannot identify which bytes survived in
the card's cache. Never call `prepare()` or `finish()` concurrently with submits.

There is exactly one submitting task and one disk task. The disk task calls
`pump()`; the acquisition task must not. `buffer()` remains for the existing
ADS1256 interrupt binding and diagnostic generator. It exposes a writer object,
not mutable ring memory; new task-based acquisition should use `submit()`.

The disk task and its status translation are project integration code.
To reuse the byte backend elsewhere, copy `lib/bufferedWriter`, implement
`byteSink::write/flush`, supply a monotonic microsecond clock and memory allocator,
and call `pump()` from that application's storage task. No Feather code is needed.

## Configuration and current sample format

`platformio.ini` selects RAM placement, ring capacity and write/transfer sizes.
These compile-time settings must be identical across all translation units.
The successful reference environment is `sd-reliability-preallocated`:
4 MiB PSRAM ring, 128 KiB writes and adapter transfers, 10 MHz four-bit SDMMC.
It still runs synthetic data and starts stopped. It is not an ADC/DMA build.

`recordingConfig::reservePayloadBytes` is selected by
`S3_PREALLOCATED_PAYLOAD_BYTES`. Nonzero reserves one contiguous exFAT file,
including an additional 512-byte header, before capture. Failure rejects Start;
there is no fallback to growing allocation. Submissions cannot extend beyond the
reservation. Stop releases the unused tail of that new file. Existing recordings
are never overwritten. Zero retains legacy growing files with approximately
1-GiB parts. A field-deployed indefinite recording policy is still to be chosen.

The reference reservation is 5,850,000,000 payload bytes: 26,000,000 bits/second
divided by eight, multiplied by 1,800 seconds. The generator, not the reservation,
owns the 30-minute stop. Free space is scanned before and after recording only.

The byte backend already accepts arbitrary byte counts. The **current file
adapter and status protocol still describe signed four-byte sample words**.
Packed three-byte storage requires a formatter plus a matching file-format ID,
header width, sample-count conversion and tests. Do not silently change the
encoding while retaining the old header. No DMA acquisition or packing is added
by this cleanup.

## Reference result and limits

2026-10-07: the preallocated reference completed 30 minutes at 26 Mbps, saving
5.85 GB with zero reported losses or write errors. Peak ring use was 2,180,928
bytes of 4,194,304; maximum write time was 299.158 ms. Final header, file size and
last sample checks passed. There was no full-payload readback or simultaneous
ADC/DMA capture. The earlier non-preallocated run overflowed after 118.327 seconds.
The card is a SanDisk Extreme 64 GB V30 A2.

Hardware tests and host launchers are under `../../test`; logs remain in ignored
`../../.pio`. Cleanup is build/test verified and has not been uploaded, so the
physical board remains on the previously tested firmware.
