# Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
"""Validate timed-format completion against independently parsed PPS boundaries."""
import re


def parseSaved(line):
    """Return the board's completed readback report; malformed rows raise errors."""
    match = re.fullmatch(
        r'TIMED SAVED run=(\d+) directory=(\S+) files=(\d+) intervals=(\d+) samples=(\d+) '
        r'bytes=(\d+) crc=([0-9a-fA-F]{8}) stagePeak=(\d+) ringPeak=(\d+) '
        r'maxWriteUs=(\d+) errors=(\d+) unsaved=(\d+) syntheticUtc=1', line)
    if not match:
        raise ValueError('Malformed TIMED SAVED row')
    names = ('run', 'directory', 'files', 'intervals', 'samples', 'bytes', 'crc',
             'stagePeak', 'ringPeak', 'maxWriteUs', 'errors', 'unsaved')
    result = dict(zip(names, match.groups()))
    for name in names:
        if name not in ('directory', 'crc'):
            result[name] = int(result[name])
    return result


def validateSaved(saved, completed, sampleCount):
    """Check the 60-second bench file: complete intervals, one file, exact bytes.

    Board readback compares every serialized byte by CRC. Here the independent
    boundary report supplies expected sample counts and therefore file size.
    Partial startup/shutdown intervals are intentionally excluded. No UTC claim.
    """
    intervals = completed.get('intervals', [])
    samples = sum(block['sCount'] for block in intervals)
    expectedBytes = 256 + len(intervals) * 48 + samples * 3 + 48
    valid = (completed['timingPassed'] and 58 <= len(intervals) <= 60 and
             saved['run'] == completed['run'] and saved['files'] == 1 and
             saved['intervals'] == len(intervals) and saved['samples'] == samples and
             saved['bytes'] == expectedBytes and saved['errors'] == saved['unsaved'] == 0 and
             0 < saved['stagePeak'] <= 2097152 and 0 < saved['ringPeak'] <= 16777216 and
             sampleCount is not None and sampleCount[0] == completed['run'] and
             intervals[-1]['firstSample'] + intervals[-1]['sCount'] <= sampleCount[1])
    if not valid:
        raise ValueError('Timed file counts do not match complete PPS intervals or report a storage fault')
