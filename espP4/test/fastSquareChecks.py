# Copyright (C) 2026 Karl Stock
# SPDX-License-Identifier: GPL-3.0-or-later
"""PC-only 1 kHz response checks. No ports, firmware or hardware are operated."""
import tempfile
import unittest
from pathlib import Path
from externalGeneratorChecks import externalFixture
from hardwareSmokeRetainedReport import analyzeRetained


def fastFixture(phase=37, period=1024, offset=0, stuck=False, overshoot=False):
    """Create fake serial-report rows containing a known square wave.

    phase shifts its starting position by samples; period is samples per cycle.
    offset adds volts to both levels. stuck holds the signal low; overshoot adds
    50 mV to the first five samples of each high section. No hardware is used."""
    feather,p4=externalFixture()
    p4=p4.replace('SMOKE_P4 version=7','SMOKE_P4 version=8')
    rows=[p4,'SMOKE_RAW_BEGIN version=1 firstSample=1024000 count=5120 rate=1024000']
    codes=[]
    for iSample in range(5120):
        position=(iSample+phase)%period
        high=position>=period//2 and not stuck
        value=(1.855 if high else 1.355)+offset
        if overshoot and high and position<period//2+5:
            value+=.05
        codes.append(round(value*8388608/2.5))
    for iSample in range(0,5120,64):
        rows.append(f'SMOKE_RAW,{iSample},'+','.join(map(str,codes[iSample:iSample+64])))
    rows.append('SMOKE_RAW_END count=5120')
    return feather,'\n'.join(rows)


class FastSquareChecks(unittest.TestCase):
    """Check known waveforms and confirm that damaged serial reports are rejected."""

    def analyze(self,**kwargs):
        """Pass the generated report through the same analysis used for a real recording."""
        return analyzeRetained(*fastFixture(**kwargs),scopeLevels=(1.355,1.855),frequencyHz=1000)

    def testStartingPhases(self):
        """Starting high, low or on an edge must not change measured frequency or levels."""
        for phase in (0,1,37,511,512,1000):
            with self.subTest(phase=phase):
                result=self.analyze(phase=phase)
                self.assertTrue(result['passed'])
                self.assertAlmostEqual(result['frequencyHz'],1000)
                self.assertAlmostEqual(result['measurements']['low']['meanV'],1.355,places=6)
                self.assertAlmostEqual(result['measurements']['high']['meanV'],1.855,places=6)

    def testWrongOrAbsentSignal(self):
        """A clean recording cannot pass with a stuck signal or wrong frequency."""
        self.assertFalse(self.analyze(stuck=True)['passed'])
        self.assertFalse(self.analyze(period=2048)['passed'])
        self.assertFalse(self.analyze(offset=.075)['passed'])

    def testOvershoot(self):
        """Report the inserted five-sample overshoot without changing the flat-section means."""
        result=self.analyze(overshoot=True)
        rising=[edge for edge in result['edges'] if edge['rising']]
        self.assertTrue(result['passed'])
        self.assertAlmostEqual(max(edge['excursionMv'] for edge in rising),50,places=3)
        self.assertGreater(max(edge['settlingUs'] for edge in rising),0)

    def testCorruptedEvidence(self):
        """Reject missing chunks, duplicate offsets, wrong rate and wrong firmware."""
        feather,p4=fastFixture()
        for before,after in [('SMOKE_RAW,64,','SMOKE_RAW,0,'),
                             ('firstSample=1024000 count=5120 rate=1024000','firstSample=1024000 count=5120 rate=1024001'),
                             ('SMOKE_RAW_END count=5120','SMOKE_RAW_END count=5119'),
                             ('SMOKE_P4 version=8','SMOKE_P4 version=7'),
                             ('overflow=0','overflow=1')]:
            with self.subTest(before=before),self.assertRaises(ValueError):
                analyzeRetained(feather,p4.replace(before,after),scopeLevels=(1.355,1.855),frequencyHz=1000)

    def testGraphOnFailedSignal(self):
        """Keep valid raw data visible even if the generator was left at a fixed level."""
        with tempfile.TemporaryDirectory() as directory:
            target=Path(directory)/'comparison.html'
            result=analyzeRetained(*fastFixture(stuck=True),scopeLevels=(1.355,1.855),frequencyHz=1000,plotPath=target)
            self.assertFalse(result['passed'])
            self.assertIn('FAIL',target.read_text())
            self.assertIn('individual ADC samples',target.read_text())


if __name__=='__main__':
    unittest.main()
