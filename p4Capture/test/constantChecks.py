"""PC-only checks for constant-report math and refusal of invalid captures."""
import tempfile
from pathlib import Path
import unittest
from constantReport import analyzeConstant, fluctuationSummary, calibrationMapping, voltagePanels


def reports():
    """Construct two complete, flat, matching captures with real report formats."""
    feather = ['DAC MODE: CONSTANT code=135 nominalVolts=1.747;',
               'INPUT_BEGIN run=42 count=300 clean=1 missed=0 maxGapUs=1000 mode=burst']
    feather += [f'INPUT,{i},{1000250+i*1000},1800,1790,1810,10,500' for i in range(300)]
    feather += ['INPUT_END run=42']
    p4 = ['WAVE_BEGIN run=42 rate=1024000 bins=600 samplesPerBin=512 clean=1']
    p4 += [f'WAVE,{i},5799990,5800010,5800000' for i in range(600)]
    p4 += ['WAVE_END run=42']
    return '\n'.join(feather), '\n'.join(p4)


class ConstantChecks(unittest.TestCase):
    """Check known fluctuation values and prevent sine/unclean data masquerading as DC."""

    def testStatistics(self):
        """Known +/-1 fluctuations have peak-to-peak 2 and RMS 1."""
        self.assertEqual(fluctuationSummary([9,11,9,11]), (10,2,1))

    def testCalibration(self):
        """Interpolation preserves known slopes, identifies fallback and rejects wrong runs."""
        text = 'INPUT_CAL run=42 source=2 defaultVrefMv=1100\n'
        text += '\n'.join(f'CAL,{raw},{raw}' for raw in list(range(0,4096,32))+[4095])
        convert,label = calibrationMapping(text,42)
        self.assertEqual(convert(1800.5),1800.5)
        self.assertIn('ESTIMATE',label)
        with self.assertRaises(ValueError):
            calibrationMapping(text,43)
        with self.assertRaises(ValueError):
            calibrationMapping(text.replace('CAL,32,32','CAL,32,0').replace('CAL,64,64','CAL,64,0').replace('CAL,96,96','CAL,96,-1'),42)
        self.assertIsNone(calibrationMapping('',42)[0])

    def testSharedScale(self):
        """Identical voltage deviations from different count scales draw identical curves."""
        points = [(0,-1),(1,1)]
        panels = [('Feather','counts',points,[],[]),
                  ('P4','counts',[(t,v*8388608/2500) for t,v in points],[],[])]
        body = voltagePanels(panels,lambda value:value,'test calibration')
        import re
        curves = re.findall(r'<polyline points="([^"]+)"',body)
        self.assertEqual(len(curves),2)
        self.assertEqual(curves[0],curves[1])

    def testFlatAndRejections(self):
        """Flat signals need no fit; missing identity, mismatched run or fault must fail."""
        feather, p4 = reports()
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder)/'constant.html'
            analyzeConstant(feather,p4,target)
            self.assertIn('averaged-point peak-to-peak=0.000',target.read_text(encoding='utf-8'))
            for inputText,outputText in [
                (feather.replace('DAC MODE: CONSTANT','DAC MODE: SINE'),p4),
                (feather,p4.replace('run=42','run=43')),
                (feather.replace('clean=1','clean=0'),p4)]:
                with self.assertRaises(ValueError):
                    analyzeConstant(inputText,outputText,target)


if __name__ == '__main__':
    unittest.main()
