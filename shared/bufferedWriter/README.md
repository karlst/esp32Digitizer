# Reusable byte writer

Copyright (C) 2026 Karl Stock. GPL-3.0-or-later; see the repository LICENSE.

This library contains the ring buffer and the code that drains it to storage.
It is used by the current P4 recorder and the legacy S3 recorder. It moved from
`espS3/lib/bufferedWriter` so P4 no longer depends on the S3 project.

Read `src/byteSink.h` for the storage interface, then `src/bufferedWriter.h`
for the calling rules and `src/bufferedWriter.cpp` for the implementation.
The library does not select card pins, interpret ADC samples, or define a file
format. Those decisions belong to each project's adapter.

P4's `main/CMakeLists.txt` compiles the library directly. S3's `platformio.ini`
discovers it through `lib_extra_dirs = ../shared`. Keep buffer size and memory
configuration consistent between the library and every source file using it.

The `test/` directory contains PC-only checks for ordering, ring wraparound,
overflow, allocation failure, partial writes, and flush failure. Run them through
`espP4/test/runDecoderChecks.ps1` or `espS3/test/runNativeChecks.ps1`.
The S3 runner adds its own sample-format and card-space checks. These tests do
not operate a board or establish physical card performance.
