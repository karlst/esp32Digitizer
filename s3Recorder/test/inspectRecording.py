"""Inspect copied SD recording files without changing them.

Usage: python inspectRecording.py recording_000001_001.bin [...]
Validate header/size/signed-24-bit range with bounded memory. This checks stored
encoding, not lossless timing or correspondence to an independently measured input.
"""
import argparse
import struct
from pathlib import Path


def inspect(path):
    """Validate one part and report its first/last raw sample and completion state."""
    # Open read-only: this is for copied recordings on the PC, not a command to
    # the S3. A malformed file raises ValueError rather than returning success.
    with path.open("rb") as source:
        header = source.read(512)
        if len(header) != 512 or header[:8] != b"S3REC001":
            raise ValueError("missing recording header")
        # struct format < means little endian; I is unsigned 32-bit and Q is
        # unsigned 64-bit. Offsets match shared/recordingFileFormat.md.
        size, version, encoding, rate, width, disposition = struct.unpack_from("<6I", header, 8)
        payload, first_index, session, part = struct.unpack_from("<QQII", header, 32)
        if (size, version, encoding, width) != (512, 1, 1, 4) or disposition not in (0, 1, 2):
            raise ValueError("unsupported header/format")
        # Trust neither the closing header nor file length alone: a completed
        # file must agree in both places. An interrupted file may retain the
        # original open header, so its payload count can legitimately be stale.
        actual = path.stat().st_size - 512
        if disposition == 1 and (actual != payload or actual % 4):
            raise ValueError("normally closed header disagrees with payload length")
        first = last = None
        remaining = actual - actual % 4
        # Read in bounded chunks, ignoring an incomplete final word. Check
        # signed range, not voltage calibration or continuous ADC timing.
        while remaining:
            block = source.read(min(65536, remaining))
            if not block or len(block) % 4:
                raise ValueError("truncated sample data")
            for (value,) in struct.iter_unpack("<i", block):
                if not -8388608 <= value <= 8388607:
                    raise ValueError("value outside signed 24-bit range")
                if first is None:
                    first = value
                last = value
            remaining -= len(block)
        return dict(session=session, part=part, rate=rate, disposition=disposition,
                    samples=actual // 4, firstIndex=first_index, first=first, last=last,
                    trailingBytes=actual % 4)


def main():
    """Print verified metadata; return a failing exit status for malformed files."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="+", type=Path)
    args = parser.parse_args()
    for path in args.files:
        print(path.name, inspect(path))


if __name__ == "__main__":
    main()
