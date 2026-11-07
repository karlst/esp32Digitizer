# Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
"""Known relative timing and damaged-stream checks; no ports or boards.

makeReport creates the same physical edges on two simulated pins with a chosen
signed difference in timer ticks. Tests cover timer wrap, one allowed boundary
omission and forbidden internal losses. No result here measures real input skew.
"""
import tempfile
from pathlib import Path
import unittest
from inputSkewReport import analyze, tickDifference, writeReport


def makeReport(skew=1, omit1=(), omit6=(), count=200):
    """Create ~997-Hz edges crossing timer wrap, with known GPIO1 minus GPIO6.

    skew is in 6.25-ns ticks. omit1/omit6 name physical edge indexes to omit;
    exported indexes are renumbered so count consistency alone cannot reveal
    the gap. count is the original number of physical transitions on each pin.
    """
    lines = []
    counts = {}
    for gpio, omitted in ((1, omit1), (6, omit6)):
        index = 0
        for iEdge in range(count):
            if iEdge not in omitted:
                ticks = (0xffff0000 + iEdge * 80240 + (skew if gpio == 1 else 0)) & 0xffffffff
                lines.append(f'SKEW EDGE run=9 gpio={gpio} index={index} ticks={ticks} rising={int(iEdge % 2 == 0)}')
                index += 1
        counts[gpio] = index
    lines.append(f'SKEW RESULT run=9 status=READY count1={counts[1]} count6={counts[6]} overflow=0')
    return lines


class InputSkewChecks(unittest.TestCase):
    """Ensure a missing half-cycle cannot masquerade as a measured input delay."""

    def testKnownSignedSkew(self):
        """Preserve positive, negative and zero skew on both polarities at wrap."""
        for skew in (-2, -1, 0, 1, 2):
            with self.subTest(skew=skew):
                result = analyze(makeReport(skew))
                self.assertEqual(len(result['pairs']), 200)
                for polarity in ('rising', 'falling'):
                    self.assertEqual(result[polarity]['meanNs'], skew * 6.25)
                    self.assertEqual(result[polarity]['stdevNs'], 0)
        self.assertEqual(tickDifference(0, 0xffffffff), 1)
        self.assertEqual(tickDifference(0xffffffff, 0), -1)

    def testBoundaryTrim(self):
        """Allow one unmatched edge from successive start/stop calls, reporting it."""
        result = analyze(makeReport(omit1=(0,), omit6=(199,)))
        self.assertEqual(len(result['pairs']), 198)
        self.assertEqual(result['leadingTrim'], 1)
        self.assertEqual(result['trailingTrim'], 1)
        self.assertEqual(result['rising']['meanNs'], 6.25)
        self.assertEqual(analyze(makeReport(omit6=(0,)))['leadingTrim'], 1)

    def testRejectMissingOrWrongEdges(self):
        """Internal losses and multiple missing boundary events must fail."""
        for kwargs in ({'omit1': (50,)}, {'omit6': (70, 71)}, {'omit1': (0, 1)},
                       {'omit6': (198, 199)}, {'count': 0}, {'count': 50}, {'skew': 2000}):
            with self.subTest(case=kwargs):
                with self.assertRaises(ValueError):
                    analyze(makeReport(**kwargs))

    def testRejectCorruption(self):
        """Check counts, polarity, range, run identity, duplication and overflow."""
        text = '\n'.join(makeReport())
        for old, new in [('overflow=0', 'overflow=1'), ('status=READY', 'status=FAIL'),
                         ('count1=200', 'count1=199'), ('gpio=1 index=0', 'gpio=1 index=1'),
                         ('rising=1', 'rising=0'), ('SKEW EDGE run=9', 'SKEW EDGE run=8')]:
            with self.subTest(change=new):
                with self.assertRaises(ValueError):
                    analyze(text.replace(old, new).splitlines())
        with self.assertRaises(ValueError):
            analyze(makeReport() + [makeReport()[-1]])

    def testReport(self):
        """Report includes signed direction, units and common-delay limitation."""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'comparison.html'
            writeReport(path, [analyze(makeReport(-1))])
            text = path.read_text(encoding='utf-8')
            self.assertIn('-6.250', text)
            self.assertIn('Positive means GPIO1', text)
            self.assertIn('common input delay cancels', text)


if __name__ == '__main__':
    unittest.main()
