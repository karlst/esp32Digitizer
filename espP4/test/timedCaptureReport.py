# Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
"""Validate timed-format completion against independently parsed PPS boundaries."""
import re
from datetime import datetime, timedelta, timezone
from fractions import Fraction


def validateNames(lines, saved, completed):
    """Check each retained path against its first sample's independently calculated UTC.

    Version-three bench starts its opening PPS at 1970-01-01 01:00:00 UTC.
    Use exact arithmetic, including the 2553-ns correction, before flooring to
    whole seconds. Require one successful measured rename for every fresh slot.
    The board separately reads back these paths and checks all serialized bytes.
    """
    paths = {}
    renames = {}
    for line in lines:
        if line.startswith('TIMED FILE '):
            match = re.fullmatch(r'TIMED FILE slot=(\d+) path=(\S+)', line)
            if not match or int(match[1]) in paths:
                raise ValueError('Malformed or duplicate timed filename report')
            paths[int(match[1])] = match[2]
        if line.startswith('TIMED IO ') and 'operation=rename ' in line:
            match = re.fullmatch(
                r'TIMED IO slot=(\d+) operation=rename calls=1 totalUs=(\d+) maxUs=(\d+) '
                r'atUs=\d+ offset=0 requested=0 actual=0 result=0 failures=0', line)
            if not match or int(match[1]) in renames:
                raise ValueError('Missing, duplicate or failed rename measurement')
            renames[int(match[1])] = int(match[3])
    expectedSlots = set(range(saved['files']))
    if set(paths) != expectedSlots or set(renames) != expectedSlots:
        raise ValueError('Not every retained file has a name and successful rename')
    results = []
    for iSlot in range(saved['files']):
        iBlock = iSlot * 60
        block = completed['intervals'][iBlock]
        offset = Fraction(block['tcFsFirst'] - block['tcPpsFirst'],
                          block['tcPpsLast'] - block['tcPpsFirst'])
        seconds = 3600 + iBlock + offset - Fraction(2553, 1_000_000_000)
        wholeSeconds = seconds.numerator // seconds.denominator
        utc = datetime(1970, 1, 1, tzinfo=timezone.utc) + timedelta(seconds=wholeSeconds)
        expected = saved['directory'] + utc.strftime('/LXD%Y%m%d-%H%M%S.bin')
        if paths[iSlot] != expected:
            raise ValueError(f'File {iSlot} name does not match its first saved sample: {paths[iSlot]}')
        results.append({'slot': iSlot, 'path': paths[iSlot], 'renameMaxUs': renames[iSlot]})
    return results


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


def validateSaved(saved, completed, sampleCount, captureSeconds=60):
    """Check complete intervals, 60-pair file rotation and exact aggregate bytes.

    Board readback compares every serialized byte by CRC. Here the independent
    boundary report supplies expected sample counts and therefore file size.
    captureSeconds selects the expected run length; default 60 retains checks
    for older bench transcripts. The five-minute runner supplies 300. Each file
    contributes its own 256-byte header and 48-byte end marker. Allow one edge
    either side of the usual duration-minus-one interval count for generator
    phase and clock difference. Partial intervals are excluded. No UTC claim.
    """
    intervals = completed.get('intervals', [])
    samples = sum(block['sCount'] for block in intervals)
    expectedFiles = (len(intervals) + 59) // 60
    expectedBytes = expectedFiles * (256 + 48) + len(intervals) * 48 + samples * 3
    valid = (completed['timingPassed'] and captureSeconds - 2 <= len(intervals) <= captureSeconds and
             saved['run'] == completed['run'] and saved['files'] == expectedFiles and
             saved['intervals'] == len(intervals) and saved['samples'] == samples and
             saved['bytes'] == expectedBytes and saved['errors'] == saved['unsaved'] == 0 and
             0 < saved['stagePeak'] <= 2097152 and 0 < saved['ringPeak'] <= 16777216 and
             sampleCount is not None and sampleCount[0] == completed['run'] and
             intervals[-1]['firstSample'] + intervals[-1]['sCount'] <= sampleCount[1])
    if not valid:
        raise ValueError('Timed file counts do not match complete PPS intervals or report a storage fault')
