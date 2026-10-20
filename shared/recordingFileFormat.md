# Recording file format

The active P4 format is defined in
[`recordingFormat.h`](../espP4/main/include/recordingFormat.h).
ADC recordings contain a 512-byte header followed by unaveraged signed 24-bit
samples stored in three little-endian bytes each. The separate synthetic storage
test uses its own format identifier and four-byte values.

See [P4 recording documentation](../espP4/README.md) for header versions,
readback checks, and limitations. The removed recorder format is no longer
documented here; its original specification remains in Git history.
