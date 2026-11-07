# Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic known-delay, corruption and missing-edge tests; no board access.

Reading path: makeReport constructs independently timed ramp responses; the
unittest methods feed those transcripts into pulseDelayReport.analyze. The known
answer is 2500 ns, not a copied measured board value. Passing checks arithmetic
and rejection rules; it cannot prove analog response shape or GPIO accuracy.
"""
import math
import tempfile
import unittest
from pathlib import Path
from pulseDelayReport import analyze, writeReport


def makeReport(phase=0.1, edgeCount=20, frequencyHz=1000):
    """Generate independent 2.5-us ramp responses across a timer wrap.

    Four sample periods span each ramp, making both its true center and its
    half-height exactly known even when the pulse falls between sample times.
    Distinct physical pulse times and stored indexes exercise the full parser.

    phase is a fraction of an ADC sample interval; edgeCount counts both rising
    and falling transitions; frequencyHz sets their spacing. Return text rows in
    the firmware's console format, including valid FSYNC mapping and summary.
    """
    base = 0xffff0000
    # Start near 32-bit wrap. Exact 625/4 ticks per sample represents 160 MHz
    # divided by 1.024 MHz; alternating steps are at known physical edge times.
    lines = ['FSYNC COUNTS run=7 hardware=40000 frames=39990 words=39989 tail=10 rejected=80',
             'FSYNC ALIGN run=7 discarded=1 clocks=31 firstPulse=2']
    for sample in (0, 13000, 26000, 39000):
        ticks = (base + sample * 625 // 4) & 0xffffffff
        lines.append(f'FSYNC ANCHOR run=7 sample={sample} pulse={sample + 2} ticks={ticks} word=08123456')
    lines.append('FSYNC RESULT run=7 status=PASS anchors=4 ticksPerSample=156.250000000 residualTicks=0.000 startup=1 clean=1')
    for iEdge in range(edgeCount):
        elapsed = round((100 + iEdge * 1024000 / (2 * frequencyHz) + phase) * 156.25)
        position = elapsed / 156.25
        start = math.floor(position) - 12
        rising = iEdge % 2 == 0
        words = []
        for sample in range(start, start + 40):
            # Generate the voltage from time, rather than from the analysis
            # formula being tested. Clip a centered linear ramp to two plateaus
            # and encode it in the real status-plus-signed-24-bit word format.
            relativeNs = (sample * 156.25 - elapsed) * 6.25
            ramp = max(0, min(1, (relativeNs - 2500) / (4 * 976.5625) + 0.5))
            volts = 0.2 + 1.6 * (ramp if rising else 1 - ramp)
            word = 0x08000000 | round(volts * 8388608 / 2.5)
            words.append(f'{word:08x}')
        lines.append(f'DELAY EDGE run=7 index={iEdge} ticks={(base + elapsed) & 0xffffffff} '
                     f'rising={int(rising)} start={start} words={"".join(words)}')
    lines.append(f'DELAY RESULT run=7 status=READY captured={edgeCount} exported={edgeCount} overflow=0')
    return lines


class PulseDelayChecks(unittest.TestCase):
    """Known mathematical responses do not establish real circuit accuracy."""

    def testTenKilohertz(self):
        """Recover delay at 10 kHz with fractional sample spacing and bounded export.

        The firmware may capture thousands of edges but exports only 128
        consecutive windows. That intentional limit must not look like loss.
        """
        lines = makeReport(edgeCount=128, frequencyHz=10000)
        lines[-1] = lines[-1].replace('captured=128', 'captured=1400')
        result = analyze(lines, frequencyHz=10000)
        self.assertAlmostEqual(result['measuredFrequencyHz'], 10000, delta=0.1)
        for direction in ('rising', 'falling', 'halfHeight'):
            self.assertAlmostEqual(result[direction]['medianNs'], 2500, delta=0.1)

        # A 10-kHz stimulus must fail if the operator requested the 1-kHz test.
        with self.assertRaises(ValueError):
            analyze(lines)
        with self.assertRaises(ValueError):
            analyze(makeReport(), frequencyHz=10000)

        # An internal lost event still fails even when the export count agrees.
        lines = [line for line in lines if not line.startswith('DELAY EDGE run=7 index=10 ')]
        lines[-1] = lines[-1].replace('exported=128', 'exported=127')
        with self.assertRaises(ValueError):
            analyze(lines, frequencyHz=10000)

    def testKnownDelay(self):
        """Recover both edge polarities at several unrelated sample phases."""
        for phase in (0.01, 0.25, 0.5, 0.9):
            with self.subTest(phase=phase):
                result = analyze(makeReport(phase))
                for direction in ('rising', 'falling', 'halfHeight'):
                    self.assertAlmostEqual(result[direction]['medianNs'], 2500, delta=0.1)

    def testRejectDamagedReports(self):
        """Incomplete transfer, bad association and lost direct edges must fail."""
        text = '\n'.join(makeReport())
        for old, new in [('overflow=0', 'overflow=1'), ('status=READY', 'status=FAIL'),
                         ('exported=20', 'exported=19'), ('index=1 ', 'index=0 '),
                         ('rising=0', 'rising=1'), ('DELAY EDGE run=7', 'DELAY EDGE run=8'),
                         ('pulse=2 ', 'pulse=1 '), ('clean=1', 'clean=0')]:
            with self.subTest(change=new):
                with self.assertRaises(ValueError):
                    analyze(text.replace(old, new).splitlines())
        lines = makeReport(edgeCount=22)
        # Remove one edge while changing the summary to look internally counted.
        # Enough edges remain; the missing middle event must fail continuity.
        lines = [line for line in lines if not line.startswith('DELAY EDGE run=7 index=10 ')]
        lines[-1] = lines[-1].replace('exported=22', 'exported=21')
        with self.assertRaises(ValueError):
            analyze(lines)

    def testRejectFlatInput(self):
        """A valid direct pulse cannot substitute for a missing analog branch."""
        lines = makeReport()
        for iLine, line in enumerate(lines):
            if line.startswith('DELAY EDGE '):
                lines[iLine] = line.split('words=')[0] + 'words=' + '08123456' * 40
        with self.assertRaises(ValueError):
            analyze(lines)

    def testRejectWrongWindow(self):
        """Demonstrate sensitivity to a one-sample labeling error, not rejection.

        The historical method name says Reject, but this deliberately plausible
        window passes waveform checks and shifts the answer by 976.5625 ns.
        Correct FSYNC association is therefore required independently.
        """
        lines = makeReport()
        changed = []
        for line in lines:
            if line.startswith('DELAY EDGE '):
                fields = line.split()
                start = int(fields[6].split('=')[1])
                line = line.replace(f'start={start} ', f'start={start + 1} ')
            changed.append(line)
        result = analyze(changed)
        self.assertAlmostEqual(result['rising']['medianNs'], 2500 + 976.5625, delta=0.1)

    def testReport(self):
        """The standalone report includes units and calibration limitations."""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'comparison.html'
            writeReport(path, [analyze(makeReport())])
            text = path.read_text(encoding='utf-8')
            self.assertIn('2.5000', text)
            self.assertIn('Std dev (ns)', text)
            self.assertIn('uncalibrated', text)


if __name__ == '__main__':
    unittest.main()
