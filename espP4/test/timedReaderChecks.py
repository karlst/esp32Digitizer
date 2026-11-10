"""Independent byte fixtures for the LXDG reader; no board, SD card or packages.

Fixtures are packed here rather than produced by the implementation under test.
They cover the user's 110/93/103-sample example, integer timing across timer wrap,
malformed records, sample sign and a normal end before preallocated trailing data.
"""
from fractions import Fraction
from pathlib import Path
import struct
import tempfile
from timedReader import TimedReader


def fixture():
    """Return explicit little-endian metadata and signed three-byte samples."""
    data = bytearray(struct.pack('<4sHQiI', b'LXDG', 1, 1800000000, 2553, 60) + bytes(234))
    for iBlock, count in enumerate((110, 93, 103)):
        ppsFirst = 0xfffffff0 + iBlock * 160000001
        first = ppsFirst + 1000
        last = first + (count - 1) * 156
        data.extend(struct.pack('<QQQQIH', first, last, ppsFirst, ppsFirst + 160000001, count, iBlock))
        data.extend(bytes(10))
        for iSample in range(count):
            value = (-8388608, -1, 0, 8388607)[iSample % 4]
            data.extend(value.to_bytes(3, 'little', signed=True))
    data.extend(bytes(48))
    return data


def rejected(path, data):
    """Require malformed bytes to fail, without leaking a mapped file handle."""
    path.write_bytes(data)
    failed = False
    try:
        with TimedReader(path):
            pass
    except ValueError:
        failed = True
    assert failed, 'Malformed fixture was accepted'


def main():
    """Check known times and structural failures using only temporary PC files."""
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / 'example.dg'
        original = fixture()
        path.write_bytes(original + b'unused preallocated bytes')
        with TimedReader(path) as reader:
            assert [block.sCount for block in reader.blocks] == [110, 93, 103]
            assert [reader.sample(0, index) for index in range(4)] == [-8388608, -1, 0, 8388607]
            # The 23rd sample of block two is index 22, with an independently
            # calculated fractional time and measured, not nominal, PPS period.
            expected = Fraction(1800000001) + Fraction(1000 + 22 * 156, 160000001) - Fraction(2553, 1000000000)
            whole, fraction = reader.sampleTime(1, 22)
            assert whole + fraction == expected
            assert reader.usedBytes == len(original)
            assert reader.blocks[1].tcPpsFirst > 0xffffffff
        # A block can end shortly after its closing PPS. Both measured endpoints
        # remain usable; the next block must start after the previous last sample.
        shifted = bytearray(original)
        secondOffset = 256 + 48 + 110 * 3
        closingPps = 0xfffffff0 + 160000001
        struct.pack_into('<Q', shifted, 256 + 8, closingPps + 844)
        struct.pack_into('<Q', shifted, secondOffset, closingPps + 1000)
        path.write_bytes(shifted)
        with TimedReader(path) as reader:
            whole, fraction = reader.sampleTime(0, 109)
            assert whole + fraction == Fraction(1800000001) + Fraction(844, 160000001) - Fraction(2553, 1000000000)
        for length in (0, 255, 256, 303, 350, len(original) - 1):
            rejected(path, original[:length])
        for offset, value in ((0, ord('X')), (1, ord('Y')), (2, ord('Z')), (3, ord('H')), (4, 2), (22, 1), (256 + 38, 1), (256 + 36, 1)):
            damaged = bytearray(original)
            damaged[offset] = value
            rejected(path, damaged)
        damaged = bytearray(original)
        struct.pack_into('<I', damaged, 256 + 32, 0xffffffff)
        rejected(path, damaged)
        # Earlier bench files used a two-byte DG identifier and different
        # header offsets. Reject them instead of interpreting their UTC wrongly.
        legacyHeader = struct.pack('<2sHQiI', b'DG', 1, 1800000000, 2553, 60) + bytes(236)
        rejected(path, legacyHeader + original[256:])

        # PPS continuity is checked at record boundaries, not by searching data.
        damaged = bytearray(original)
        secondBlock = 256 + 48 + 110 * 3
        struct.pack_into('<Q', damaged, secondBlock + 16, 0xfffffff0 + 160000000)
        rejected(path, damaged)
    print('PASS: timed-file layout, signed samples, exact times, wraps, end marker and rejection checks')


if __name__ == '__main__':
    main()
