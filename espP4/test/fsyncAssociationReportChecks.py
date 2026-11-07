# Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic report checks; no serial ports, hardware or SD access.

makeReport supplies valid startup/count evidence and a known timer-to-sample
ratio. Each test changes a specific claim while retaining the rest. The parser
must reject conflicting evidence even if the text still says PASS. Hardware
startup is checked separately; these fixtures cannot reproduce electrical edges.
"""
import unittest
from fsyncAssociationReport import analyze


def makeReport():
    """Construct an independently numbered, exact 160-MHz/1.024-MHz example."""
    lines = ['FSYNC COUNTS run=7 hardware=40000 frames=39990 words=39989 tail=10 rejected=80',
             'FSYNC ALIGN run=7 discarded=1 clocks=31 firstPulse=2']
    for sample in (0, 1024, 2048, 3072):
        ticks = (0xffff0000 + sample * 625 // 4) & 0xffffffff
        lines.append(f'FSYNC ANCHOR run=7 sample={sample} pulse={sample + 2} ticks={ticks} word=08123456')
    lines.append('FSYNC RESULT run=7 status=PASS anchors=4 ticksPerSample=156.250000000 residualTicks=0.000 startup=1 clean=1')
    return lines


class FsyncReportChecks(unittest.TestCase):
    """Reject plausible-looking PASS lines whose underlying evidence disagrees."""

    def testTimerWrap(self):
        """Parse wrapped timestamps and reported totals above the counter limit.

        This checks the report parser, not the hardware PCNT wrap interrupt.
        Counter-extension arithmetic is exercised in decoderChecks.cpp.
        """
        result = analyze(makeReport())
        self.assertTrue(result['passed'])
        self.assertEqual(result['anchors'][0]['sample'], 0)
        self.assertEqual(result['ticksPerSample'], 156.25)
        self.assertEqual(result['anchors'][0]['pulse'], 2)
        self.assertEqual(result['discardedStartupFrames'], 1)
        self.assertTrue(analyze([line.replace('clocks=31', 'clocks=32') for line in makeReport()])['passed'])

    def testRejectDamagedReports(self):
        """Missing records, wrong numbering and fault flags must prevent PASS."""
        text = '\n'.join(makeReport())
        replacements = [('status=PASS', 'status=FAIL'), ('startup=1', 'startup=0'),
                        ('clean=1', 'clean=0'), ('sample=1024', 'sample=1025'),
                        ('sample=2048', 'sample=1024'), ('tail=10', 'tail=11'),
                        ('hardware=40000', 'hardware=39989'), ('anchors=4', 'anchors=5'),
                        ('residualTicks=0.000', 'residualTicks=3.000'),
                        ('discarded=1', 'discarded=0'), ('clocks=31', 'clocks=30'),
                        ('firstPulse=2', 'firstPulse=1'), ('pulse=2 ', 'pulse=1 '),
                        ('words=39989', 'words=39990'),
                        ('FSYNC ANCHOR run=7 sample=0', 'FSYNC ANCHOR run=8 sample=0')]
        for old, new in replacements:
            with self.subTest(change=new):
                with self.assertRaises(ValueError):
                    analyze(text.replace(old, new).splitlines())
        with self.assertRaises(ValueError):
            analyze(makeReport()[1:])
        with self.assertRaises(ValueError):
            analyze(makeReport() + [makeReport()[-1]])

    def testRejectConstantOffset(self):
        """A one-sample shift preserves spacing but must fail pulse association."""
        lines = makeReport()
        for iLine, line in enumerate(lines):
            if line.startswith('FSYNC ANCHOR'):
                fields = line.split()
                sample = int(fields[3].split('=')[1])
                lines[iLine] = line.replace(f'sample={sample} ', f'sample={sample + 1} ')
        with self.assertRaises(ValueError):
            analyze(lines)


if __name__ == '__main__':
    unittest.main()
