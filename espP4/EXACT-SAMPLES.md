# Exact digital value test (PC only)

Karl requested a Fibonacci sequence test October 18, 2026. Run from repo root:

```powershell
.\espP4\test\runExactSampleChecks.ps1
```

No boards need to be connected. Karl runs compilation and execution. The source
was executed by Karl, who reported PASS on October 18, 2026.

The test constructs the paired FSYNC/data bytes expected from PARLIO, using
Fibonacci values starting 1,1,2,3,5,8,13,21. It stops Fibonacci before exceeding
the signed 24-bit range. Negative Fibonacci, zero, both signed limits, mixed-byte
patterns, every individual bit and its inverse, and a longer deterministic
sequence exercise values that small positive numbers would miss.

Those bytes go through the actual espP4/main/src/FrameDecoder.cpp on the PC.
The test copies every callback output into RAM and requires the same count,
order and values as the original list, with the expected status byte. It then
calls the actual recordingFormat::packSamples helper and compares all three
stored bytes of every sample with the original value. No disk writer runs.

160 cases combine two frame lengths (32/64 clocks), four bit-pair packing phases,
four source-pointer alignments, and five feed chunk patterns (1, 3, 4032 bytes,
the current P4 DMA descriptor-length sequence, or one entire input block).
Splitting feed calls is a software simulation of buffer boundaries, NOT an
execution of DMA, the interrupt copy, FreeRTOS queues or the ring buffer.

Negative controls check that value comparison detects a changed bit, reordered
samples and a dropped sample. Another control flips one serialized sample bit
without changing FSYNC or status, demonstrating why zero header errors alone
cannot establish intact sample data. Any mismatch exits with failure.

No averaging, tolerance or scale adjustment is used. Passing establishes exact
software interpretation for these cases; it does not establish ADC analog
accuracy, physical input timing, PARLIO/GDMA operation or full system data-loss
behavior. A hardware known-pattern experiment remains a separate next task.

## Physical test investigation

TI's ADS127L18 Rev B datasheet sections 7.3.1, 7.4.9.8 and 7.4.9.9 describe
analog input test selections and optional frame-sync CRC, but no programmable
digital sample generator. The internal analog tests cannot emit an exact
Fibonacci sequence. A physical known-sequence test needs a separate digital
source driving the capture signals, with an appropriate electrical connection
and verified clock timing. That arrangement has not been implemented.

CRC is a separate possible check: the ADC appends an error-detection byte and
the receiver recomputes it from received data. It cannot establish that whole
frames were never lost or repeated, and it changes frame-length requirements.
It must not be silently substituted for this exact expected-sequence test.

Source: https://www.ti.com/lit/ds/symlink/ads127L18.pdf
