# Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
"""Check that timed-format host validation rejects mismatched completion counts."""
from timedCaptureReport import parseSaved, validateSaved, validateNames


def checkRun(captureSeconds, intervalCount):
    """Model one or five files, then require corrupt summaries to be rejected.

    Expected bytes are computed here from full and partial files separately,
    including every header/end marker. Nothing accesses physical hardware.
    """
    intervals = [{'sCount': 1024000, 'firstSample': 100 + i * 1024000} for i in range(intervalCount)]
    samples = intervalCount * 1024000
    fullFiles, lastPairs = divmod(intervalCount, 60)
    files = fullFiles + bool(lastPairs)
    byteCount = fullFiles * (256 + 60 * (48 + 1024000 * 3) + 48)
    if lastPairs:
        byteCount += 256 + lastPairs * (48 + 1024000 * 3) + 48
    saved = parseSaved(f'TIMED SAVED run=42 directory=0:/TIMED/S000001 files={files} intervals={intervalCount} '
                       f'samples={samples} bytes={byteCount} crc=1234abcd stagePeak=1040000 '
                       'ringPeak=131072 maxWriteUs=120000 errors=0 unsaved=0 syntheticUtc=1')
    completed = {'run': 42, 'timingPassed': True, 'intervals': intervals}
    validateSaved(saved, completed, (42, samples + 1000), captureSeconds)
    for field in ('run', 'files', 'intervals', 'samples', 'bytes', 'errors', 'unsaved'):
        bad = dict(saved)
        bad[field] += 1
        try:
            validateSaved(bad, completed, (42, samples + 1000), captureSeconds)
        except ValueError:
            pass
        else:
            raise AssertionError(f'Accepted incorrect {field}')

    # A single-file byte total must not pass after rotation. Also reject a
    # correct short capture when the host actually requested five minutes.
    bad = dict(saved)
    bad['bytes'] = 256 + intervalCount * 48 + samples * 3 + 48
    if files > 1:
        try:
            validateSaved(bad, completed, (42, samples + 1000), captureSeconds)
        except ValueError:
            pass
        else:
            raise AssertionError('Accepted missing rotated-file headers and markers')
    if captureSeconds == 60:
        try:
            validateSaved(saved, completed, (42, samples + 1000), 300)
        except ValueError:
            pass
        else:
            raise AssertionError('Accepted a one-minute run for a five-minute request')


def checkNames():
    """Reject wrong dates and failed/missing renames, including second-boundary cases."""
    blocks = []
    for iBlock in range(299):
        pps = iBlock * 1_000_000_000
        blocks.append({'tcPpsFirst': pps, 'tcPpsLast': pps + 1_000_000_000,
                       'tcFsFirst': pps + (2552 if iBlock == 0 else 2553)})
    saved = {'files': 5, 'directory': '0:/TIMED/S000001'}
    names = ['005959', '010100', '010200', '010300', '010400']
    lines = []
    for iSlot, name in enumerate(names):
        lines.append(f'TIMED FILE slot={iSlot} path=0:/TIMED/S000001/LXD19700101-{name}.bin')
        lines.append(f'TIMED IO slot={iSlot} operation=rename calls=1 totalUs=25 maxUs=25 '
                     'atUs=0 offset=0 requested=0 actual=0 result=0 failures=0')
    completed = {'intervals': blocks}
    assert len(validateNames(lines, saved, completed)) == 5
    variants = [lines[:-1], lines + [lines[0]],
                [line.replace('005959', '010000') for line in lines],
                [line.replace('failures=0', 'failures=1') for line in lines]]
    for bad in variants:
        try:
            validateNames(bad, saved, completed)
        except ValueError:
            pass
        else:
            raise AssertionError('Accepted incorrect filename or rename report')


def main():
    """Cover nominal, partial and exactly full final files without operating boards."""
    for captureSeconds, intervalCount in ((60, 59), (300, 298), (300, 299), (300, 300)):
        checkRun(captureSeconds, intervalCount)
    checkNames()
    print('PASS: timed SD completion checks duration, rotation, bytes and sample counts')


if __name__ == '__main__':
    main()
