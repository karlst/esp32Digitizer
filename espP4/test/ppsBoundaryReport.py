"""Validate directly captured sBlock boundaries from the P4 transcript.

Each boundary contains two adjacent samples; the earlier one closes an interval
and the later one opens the next. No timestamp is extrapolated from a nominal
sample period. This parser checks reported evidence, not physical correctness.
"""
import re


def analyzeBoundaries(lines):
    """Return complete interval metadata, requiring one successful pair per PPS.

    Rows may be interspersed with storage/decoder reports. Requiring the final
    summary prevents an older firmware that reports only isolated PPS references
    from passing this new test. firstSample indexes the original decoded stream;
    it is diagnostic metadata, not an additional field in the binary tBlock.
    """
    pattern = re.compile(r'PPS BOUNDARY event=(\d+) ppsTicks64=(\d+) lastSample=(\d+) lastFsTicks64=(\d+) firstSample=(\d+) firstFsTicks64=(\d+) spacingTicks=(\d+)')
    records = []
    summary = None
    mapped = None
    for line in lines:
        match = pattern.fullmatch(line)
        if match:
            event, pps, lastSample, lastTicks, firstSample, firstTicks, spacing = map(int, match.groups())
            if firstSample != lastSample + 1 or firstTicks - lastTicks != spacing or spacing not in (156, 157):
                raise ValueError('Boundary is not two consecutive FSYNC samples')
            if not pps <= lastTicks < firstTicks <= pps + 1600000:
                raise ValueError('Boundary did not follow PPS within 10 ms')
            if records:
                previous = records[-1]
                if event != previous[0] + 1 or not 158400000 <= pps - previous[1] <= 161600000:
                    raise ValueError('Missing or invalid PPS interval at block boundary')
                if firstSample <= previous[4] + 1 or lastTicks <= previous[5]:
                    raise ValueError('Boundary sample numbering or ticks moved backward')
            records.append((event, pps, lastSample, lastTicks, firstSample, firstTicks))
        elif line.startswith('PPS BOUNDARY '):
            raise ValueError('Malformed boundary report')
        elif line.startswith('PPS BOUNDARIES '):
            summary = re.fullmatch(r'PPS BOUNDARIES captured=(\d+) failed=0 missing=0', line)
        elif line.startswith('PPS TOTAL '):
            total = re.search(r' mapped=(\d+) ', line)
            mapped = int(total[1]) if total else None
    if summary is None or len(records) != int(summary[1]) or len(records) != mapped or len(records) < 2:
        raise ValueError('Missing/failed consecutive-FSYNC boundary capture; inspect PPS BOUNDARIES')
    intervals = []
    for iInterval, (opening, closing) in enumerate(zip(records, records[1:])):
        intervals.append({
            'tcFsFirst': opening[5], 'tcFsLast': closing[3],
            'tcPpsFirst': opening[1], 'tcPpsLast': closing[1],
            'sCount': closing[4] - opening[4], 'nSec': iInterval,
            'firstSample': opening[4]})
    return intervals
