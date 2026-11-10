# Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
"""Check that timed-format host validation rejects mismatched completion counts."""
from timedCaptureReport import parseSaved, validateSaved


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


def main():
    """Cover nominal, partial and exactly full final files without operating boards."""
    for captureSeconds, intervalCount in ((60, 59), (300, 298), (300, 299), (300, 300)):
        checkRun(captureSeconds, intervalCount)
    print('PASS: timed SD completion checks duration, rotation, bytes and sample counts')


if __name__ == '__main__':
    main()
