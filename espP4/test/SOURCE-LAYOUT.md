# Recording and test source layout

The application implementation is shared by recording and tests. Only startup
and the services supplied at startup differ; there is no copied decoder, writer
or ADC configuration sequence.

| Purpose | P4 | Feather |
| --- | --- | --- |
| Ordinary startup | `main/src/main.cpp` | `src/main.cpp` |
| Real application | `main/src/RecordingApplication.cpp` | `src/AcquisitionApplication.cpp` |
| Application headers | `main/include` | `include` |
| Test startup | `test/hardware/main.cpp` | `test/hardware/ads127CaptureControl.cpp` |
| Test services | `RecordingHarness`, `RamCaptureDestination`, `SdWriteTest`, `SdSmokeSummary` | `BenchAcquisitionServices`, `BenchDac`, `InputSnapshot` |
| Profiling | `test/profile` | No separate CPU profiler |

P4 CMake selects test startup only for smoke, profiling or `CONFIG_P4_DIAGNOSTICS`.
Ordinary startup creates no test objects or generated-data task. The recorder's
layout and readback interfaces allow test-specific verification without including
test classes in the recorder. `CONFIG_P4_DIAGNOSTICS` enables manual RAM and SD
speed tests in a separate configured build; smoke/profile already include them.

Feather environment `recording` selects ordinary startup and the external ADC
input. The existing named test environments select their own startup and inject
test services. The reusable ADC configuration and SPI driver live in `src/adc`
and `include/adc`. `shared/BenchLine.h` assembles protocol lines for both boards;
shared test-duration choices remain in `shared/test/support`.

The preserved `espFeather/webApp` is a separate PlatformIO project. Its LED
service lives with its own source and headers; terminal firmware does not link
web code. Its old S3 status interface has not been converted into P4 control.
The S3 firmware project was removed; the Feather driver checks and Arduino/SPI
substitutes it formerly hosted now live in `espFeather/test`.

[Data flow](../../docs/DATA-FLOW.md) describes ownership and actual transfer calls.
[Build instructions](../../docs/BUILDING.md) list ordinary and test builds.
After source changes, Karl runs PC checks and firmware builds before uploads;
source inspection alone cannot establish compilation or hardware behavior.
