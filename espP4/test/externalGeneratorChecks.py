# Copyright (C) 2026 Karl Stock
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline external-generator report checks; no ports or devices are accessed."""
import tempfile
import unittest
from pathlib import Path
from externalGeneratorReport import validateLevels, analyzeWaveform
from hardwareSmokeReportChecks import retainedFixture
from hardwareSmokeRetainedReport import analyzeRetained
from sampleNoiseReport import addSampleStatistics, microvoltsPerCount, plotSamples


def externalFixture(seconds=30, phaseBins=3, offsetMv=0, stuck=False):
    """Reuse disk metadata but replace A2/DAC reports with external-controller evidence."""
    feather, p4 = retainedFixture(seconds)
    feather = '\n'.join(line for line in feather.splitlines()
                        if not line.startswith(('INPUT', 'DAC ', 'CAL')))
    feather = feather.replace('version=4', 'version=6') + '\nEXTERNAL_END run=7 clean=1'
    p4 = p4.replace('SMOKE_P4 version=5', 'SMOKE_P4 version=7')
    rows = []
    moments = ['SMOKE_NOISE version=1 traceSamples=128 traceBins=40']
    traceCount = 0
    for line in p4.splitlines():
        if line.startswith('SMOKE_SD,'):
            fields = line.split(',')
            iBin = int(fields[1])
            # The generator need not start low or change at capture-second boundaries.
            phase = (iBin + phaseBins) % 20
            mv = (1355 if phase < 10 or stuck else 1855) + offsetMv
            code = round(mv * 8388608 / 2500)
            fields[3] = str(code)
            line = ','.join(fields)
            moments.append(f'SMOKE_MOMENT,{iBin},0,{code},{code}')
            if traceCount < 40:
                moments.append(f'SMOKE_TRACE,{iBin},' + ','.join([str(code)] * 128))
                traceCount += 1
        rows.append(line)
    return feather, '\n'.join(rows + moments)


class ExternalGeneratorChecks(unittest.TestCase):
    """Require integrity evidence and preserve useful graphs when voltage checks fail."""

    def testPhaseAndRetention(self):
        """All initial phases and retained windows work without assumed start-low timing."""
        for seconds in (30, 300, 600):
            for phaseBins in range(20):
                with self.subTest(seconds=seconds, phase=phaseBins):
                    result = analyzeRetained(*externalFixture(seconds, phaseBins), scopeLevels=(1.355, 1.855))
                    self.assertTrue(result['passed'])
                    self.assertAlmostEqual(result['measurements']['low']['meanMv'], 1355, places=3)
                    self.assertAlmostEqual(result['measurements']['high']['meanMv'], 1855, places=3)

    def testOffsetAndGraph(self):
        """A real level error is not fitted away; a failing waveform still gets a graph."""
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / 'comparison.html'
            result = analyzeRetained(*externalFixture(offsetMv=75), plotPath=target, scopeLevels=(1.355, 1.855))
            self.assertFalse(result['passed'])
            self.assertAlmostEqual(result['measurements']['low']['errorMv'], 75, places=3)
            self.assertIn('scope readings entered', target.read_text(encoding='utf-8'))
            self.assertIn('FAIL', target.read_text(encoding='utf-8'))

    def testStuckSignal(self):
        """Clean disk data alone cannot pass with the generator off or signal stuck."""
        result = analyzeRetained(*externalFixture(stuck=True), scopeLevels=(1.355, 1.855))
        self.assertFalse(result['passed'])
        self.assertFalse(result['timingGood'])

    def testIntegrityAndIdentity(self):
        """Wrong source firmware, receive loss, bad completion and missing bins fail."""
        feather, p4 = externalFixture()
        for before, after in [('overflow=0', 'overflow=1'), ('drops=0', 'drops=1'),
                              ('clean=1', 'clean=0'), ('SMOKE_SD,20,', 'SMOKE_SD,19,')]:
            with self.subTest(before=before), self.assertRaises(ValueError):
                analyzeRetained(feather, p4.replace(before, after), scopeLevels=(1.355, 1.855))
        for before, after in [('version=6', 'version=4'), ('EXTERNAL_END run=7', 'EXTERNAL_END run=8'),
                              ('clean=1', 'clean=0')]:
            with self.subTest(before=before), self.assertRaises(ValueError):
                analyzeRetained(feather.replace(before, after), p4, scopeLevels=(1.355, 1.855))

    def testInvalidReferences(self):
        """Bad scope entries cannot become a meaningless voltage comparison."""
        for levels in ((float('nan'), 1.8), (1.8, 1.3), (1.5, 1.5), (-1, 1), (1, 3)):
            with self.subTest(levels=levels), self.assertRaises(ValueError):
                validateLevels(levels)

    def testEdgesAndVariation(self):
        """Mixed edge bins are omitted; retained plateau variation remains visible."""
        outputs = {}
        for iBin in range(300):
            phase = (iBin + 3) % 20
            mv = 1355 if phase < 10 else 1855
            if phase in (0, 10):
                mv = 1605
            else:
                mv += 2 if iBin % 2 else -2
            outputs[iBin] = mv
        result = analyzeWaveform(outputs, (1.355, 1.855), {})
        self.assertTrue(result['passed'])
        self.assertLess(abs(result['measurements']['low']['errorMv']), 1)
        self.assertGreater(result['measurements']['low']['stddevMv'], 1)

    def testIndividualSampleVariance(self):
        """Combine within-bin fluctuation with between-bin drift, without averaging it away."""
        rows = ['SMOKE_NOISE version=1 traceSamples=128 traceBins=40']
        for iBin, mean in enumerate((6000000, 6000006, 6200000, 6200006)):
            rows.append(f'SMOKE_SD,{iBin},102400,{mean}')
            rows.append(f'SMOKE_MOMENT,{iBin},409600,{mean-2},{mean+2}')
            trace = [str(mean + (2 if iSample % 2 else -2)) for iSample in range(128)]
            rows.append(f'SMOKE_TRACE,{iBin},' + ','.join(trace))
        result = dict(settledBins=dict(low=[0, 1], high=[2, 3]))
        addSampleStatistics('\n'.join(rows), result, 1024000)
        for values in result['sampleStatistics'].values():
            self.assertAlmostEqual(values['varianceUv2'], 13 * microvoltsPerCount**2)
            self.assertEqual(values['samples'], 204800)
            self.assertAlmostEqual(values['peakToPeakUv'], 10 * microvoltsPerCount)
            self.assertAlmostEqual(sum(values['traceResidualUv']), 0)
        self.assertIn('Microseconds', plotSamples(result))

    def testMissingSampleStatistics(self):
        """Old firmware or missing moments/traces must not appear to have zero noise."""
        feather, p4 = externalFixture()
        for prefix in ('SMOKE_NOISE', 'SMOKE_MOMENT,3,', 'SMOKE_TRACE'):
            changed = '\n'.join(line for line in p4.splitlines() if not line.startswith(prefix))
            with self.subTest(prefix=prefix), self.assertRaises(ValueError):
                analyzeRetained(feather, changed, scopeLevels=(1.355, 1.855))


if __name__ == '__main__':
    unittest.main()
