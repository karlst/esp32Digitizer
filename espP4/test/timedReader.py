"""Read LXDG version-one tBlock/sBlock files without third-party packages.

The file is memory-mapped, not copied into a large Python list. Sample values
are signed 24-bit integers. sampleTime returns integer UTC seconds plus an exact
fractional second: it never rounds a large Unix timestamp through a float.
The filename test supplies 3600 (1970-01-01 01:00:00 UTC) as a synthetic origin; earlier five-minute files used 946684800 (2000-01-01 UTC).
There is no on-disk GPS-validity flag: any header origin, including nonzero UTC,
must be traced to its source before treating calculated sample times as GPS time.

Reading path: parse follows header/tBlock/sample byte counts; sample retrieves
one voltage code; sampleTime converts that sample's interpolated FSYNC ticks to
UTC and subtracts the input-path delay. The two PPS values in this implementation
are named tcPpsFirst/Last; the client document calls them tcPpsCurrent/Next.
"""
import argparse
from dataclasses import dataclass
from fractions import Fraction
import mmap
from pathlib import Path
import struct


HEADER_BYTES = 256
TBLOCK_BYTES = 48
HEADER = struct.Struct('<4sHQiI234s')
TBLOCK = struct.Struct('<QQQQIH10s')


@dataclass(frozen=True)
class TimeBlock:
    """One interval's timing fields and sample-byte offset, without sample copies.

    Tick counts are extended integers from the same hardware timer, not seconds.
    tcPpsFirst/Last are consecutive opening/closing PPS counts. sampleOffset is
    a byte offset from the file start; sCount is a count of three-byte samples.
    """
    tcFsFirst: int
    tcFsLast: int
    tcPpsFirst: int
    tcPpsLast: int
    sCount: int
    nSec: int
    sampleOffset: int


class TimedReader:
    """Validate one complete file, then provide indexed sample/time access.

    Use with TimedReader(path) as reader to close the map reliably. Preallocated
    trailing space after the zero marker is ignored; a truncated header, tBlock
    or sBlock is an error. No search for a marker inside sample bytes is allowed.
    Validation checks structure and arithmetic, not GPS lock or signal fidelity.
    """

    def __init__(self, path):
        """Open read-only and parse metadata without allocating sample arrays."""
        self.file = Path(path).open('rb')
        self.mapping = None
        try:
            if self.file.seek(0, 2) < HEADER_BYTES + TBLOCK_BYTES:
                raise ValueError('File is too short for a header and end marker')
            self.mapping = mmap.mmap(self.file.fileno(), 0, access=mmap.ACCESS_READ)
            self.blocks = []
            self.parse()
        except BaseException:
            self.close()
            raise

    def parse(self):
        """Called at open to walk records and retain only their small metadata.

        Advance by 48 bytes for a tBlock and 3*sCount bytes for its samples.
        An all-zero record ends the file only where a tBlock is expected; sample
        bytes are never searched for a marker. Raise ValueError on structural
        errors. These checks cannot detect arbitrary corruption of sample data
        or establish the hardware association between FSYNC and sample numbers.
        """
        fileId, version, self.utcFirst, self.delayNs, self.pairsPerFile, reserved = HEADER.unpack_from(self.mapping)
        if fileId != b'LXDG' or version != 1:
            raise ValueError('Expected LXDG file format version 1')
        if any(reserved) or not 1 <= self.pairsPerFile <= 65535 or self.delayNs < 0:
            raise ValueError('Invalid header fields or nonzero reserved bytes')
        offset = HEADER_BYTES

        # The marker ends meaningful data; preallocated bytes beyond it are not
        # part of the recording. previous checks continuity between tBlocks.
        ended = False
        previous = None
        while not ended:
            if offset + TBLOCK_BYTES > len(self.mapping):
                raise ValueError('Missing or truncated ending tBlock')
            raw = self.mapping[offset:offset + TBLOCK_BYTES]
            offset += TBLOCK_BYTES
            if raw == bytes(TBLOCK_BYTES):
                ended = True
            else:
                first, last, ppsFirst, ppsLast, count, second, reserved = TBLOCK.unpack(raw)
                if any(reserved) or count < 2 or not (ppsFirst <= first < ppsLast and first < last):
                    raise ValueError('Invalid tBlock endpoints, count or reserved bytes')
                if second != len(self.blocks) or second >= self.pairsPerFile:
                    raise ValueError('Missing, duplicate or excessive PPS interval')
                if previous and (ppsFirst != previous.tcPpsLast or first <= previous.tcFsLast):
                    raise ValueError('Discontinuous PPS or FSYNC counts')
                end = offset + count * 3
                if end > len(self.mapping):
                    raise ValueError('Truncated sBlock')
                block = TimeBlock(first, last, ppsFirst, ppsLast, count, second, offset)
                self.blocks.append(block)
                previous = block
                offset = end
        self.usedBytes = offset

    def sample(self, iBlock, iSample):
        """Return one signed ADC value; both indices are zero-based."""
        block = self.blocks[iBlock]
        if not 0 <= iSample < block.sCount:
            raise IndexError('Sample index outside sBlock')
        offset = block.sampleOffset + iSample * 3
        return int.from_bytes(self.mapping[offset:offset + 3], 'little', signed=True)

    def sampleTime(self, iBlock, iSample):
        """Return (whole UTC seconds, Fraction of a second) for analog-input time.

        Interpolate FSYNC ticks between the block's two endpoints, divide by its
        measured PPS tick interval, then subtract the configured input delay.
        Fraction preserves sub-nanosecond arithmetic without implying equivalent
        physical accuracy. The real measurement uncertainty remains independent.
        """
        block = self.blocks[iBlock]
        if not 0 <= iSample < block.sCount:
            raise IndexError('Sample index outside sBlock')

        # There are sCount-1 gaps between the first and last samples. For three
        # samples at ticks 100, 200, 300, the spacing is (300-100)/(3-1)=100.
        # Fraction retains an exact numerator/denominator instead of rounding
        # the intermediate tick count to a float or an integer hardware tick.
        ticks = Fraction(block.tcFsFirst) + Fraction(
            (block.tcFsLast - block.tcFsFirst) * iSample, block.sCount - 1)

        # Opening PPS has UTC utcFirst+nSec. Divide the sample's tick offset by
        # the measured ticks between successive PPS pulses; this corrects the
        # block's average timer rate. Subtract delayNs/1e9 once, in seconds, to
        # obtain analog-input time. This is interpolation, not a jitter correction.
        seconds = Fraction(self.utcFirst + block.nSec) + (
            ticks - block.tcPpsFirst) / (block.tcPpsLast - block.tcPpsFirst) - Fraction(self.delayNs, 1000000000)

        # Return whole UTC seconds plus a fraction in [0,1). To obtain the
        # document's tSample in nanoseconds, the caller can use
        # (whole + fraction)*1_000_000_000, retaining Fraction arithmetic.
        whole = seconds.numerator // seconds.denominator
        return whole, seconds - whole

    def close(self):
        """Release the read-only map before closing its underlying file."""
        if self.mapping is not None:
            self.mapping.close()
            self.mapping = None
        self.file.close()

    def __enter__(self):
        """Return this validated reader for a with statement."""
        return self

    def __exit__(self, exceptionType, exception, traceback):
        """Close resources on normal completion and on exceptions."""
        self.close()


def main():
    """Validate one saved file and print counts; do not modify its contents."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('path', type=Path)
    args = parser.parse_args()
    with TimedReader(args.path) as reader:
        print(f'DG v1: {len(reader.blocks)} intervals, {sum(block.sCount for block in reader.blocks)} samples')
        print(f'UTC origin={reader.utcFirst}; delay={reader.delayNs} ns; used bytes={reader.usedBytes}')


if __name__ == '__main__':
    main()
