"""Desktop transcript checks: no serial ports, generator or hardware execution."""
from ppsBoundaryReport import analyzeBoundaries


def fixture():
    """Use three PPS events and known adjacent sample edges across a raw wrap."""
    lines = []
    for event in range(3):
        pps = 0xfffffff0 + event * 160000000
        firstSample = 1000 + event * 1024000
        lines.append(f'PPS BOUNDARY event={event} ppsTicks64={pps} lastSample={firstSample - 1} lastFsTicks64={pps + 1000} firstSample={firstSample} firstFsTicks64={pps + 1156} spacingTicks=156')
    lines.extend(['PPS BOUNDARIES captured=3 failed=0 missing=0',
                  'PPS TOTAL captured=3 mapped=3 beforeFirst=0 afterLast=0 late=0 badIntervals=0 overflow=0 numbered=1'])
    return lines


def rejected(lines):
    """Require a broken transcript to fail instead of inventing missing data."""
    failed = False
    try:
        analyzeBoundaries(lines)
    except ValueError:
        failed = True
    assert failed


def main():
    """Validate complete intervals and reject missing/duplicate/unpaired edges."""
    lines = fixture()
    intervals = analyzeBoundaries(lines)
    assert len(intervals) == 2
    assert intervals[0]['sCount'] == 1024000 and intervals[0]['firstSample'] == 1000
    assert intervals[0]['tcFsLast'] > intervals[0]['tcPpsLast']
    assert intervals[1]['tcFsFirst'] - intervals[0]['tcFsLast'] == 156
    rejected(lines[1:])
    rejected([line for line in lines if not line.startswith('PPS BOUNDARIES')])
    rejected([line.replace('failed=0', 'failed=1') for line in lines])
    rejected([line.replace('spacingTicks=156', 'spacingTicks=312') for line in lines])
    rejected([lines[0], lines[0]] + lines[1:])
    rejected([line.replace('firstSample=1000 ', 'firstSample=1001 ') for line in lines])
    print('PASS: adjacent FSYNC block boundaries, interval counts and rejection rules')


if __name__ == '__main__':
    main()
