# Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
"""Check that timed-format host validation rejects mismatched completion counts."""
from timedCaptureReport import parseSaved, validateSaved


def main():
    """Supply 59 complete intervals, then corrupt each independently checked count."""
    intervals = [{'sCount': 1024000, 'firstSample': 100 + i * 1024000} for i in range(59)]
    samples = 59 * 1024000
    byteCount = 256 + 59 * 48 + samples * 3 + 48
    saved = parseSaved(f'TIMED SAVED run=42 directory=0:/TIMED/S000001 files=1 intervals=59 '
                       f'samples={samples} bytes={byteCount} crc=1234abcd stagePeak=1040000 '
                       'ringPeak=131072 maxWriteUs=120000 errors=0 unsaved=0 syntheticUtc=1')
    completed = {'run': 42, 'timingPassed': True, 'intervals': intervals}
    validateSaved(saved, completed, (42, samples + 1000))
    for field in ('run', 'files', 'intervals', 'samples', 'bytes', 'errors', 'unsaved'):
        bad = dict(saved)
        bad[field] += 1
        try:
            validateSaved(bad, completed, (42, samples + 1000))
        except ValueError:
            pass
        else:
            raise AssertionError(f'Accepted incorrect {field}')
    print('PASS: timed SD completion matches interval-derived byte and sample counts')


if __name__ == '__main__':
    main()
