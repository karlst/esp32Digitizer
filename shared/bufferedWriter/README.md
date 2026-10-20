# Reusable byte writer

Copyright (C) 2026 Karl Stock. GPL-3.0-or-later; see the repository LICENSE.

This library owns a ring buffer and drains it through a supplied `ByteSink`.
P4 uses it directly; it depends on no ADC driver, board protocol or test harness.
Read `src/ByteSink.h`, `src/BufferedWriter.h`, then `src/BufferedWriter.cpp`.
The board adapter chooses card pins, file format and allocation policy.

P4 CMake compiles the implementation directly. Buffer configuration must match
in every translation unit using the class. Submission copies bytes into owned
RAM and never calls the disk. Storage writes release space only after success.

Run the PC checks through `espP4/test/runDecoderChecks.ps1` or the top-level
`test/runSmokeChecks.ps1`. They cover ordering, wraparound, overflow, allocation
failure, partial writes, flush failure and counter rollover beyond 4 GiB.
They do not establish board timing or card performance.
