# P4 early capture and SD test history

Historical checkpoints through October 17, 2026. For the current build and
results, read [the October 19 status](STATUS-2026-10-19-P4.md).

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

