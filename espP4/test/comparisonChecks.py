# Copyright (C) 2026 Karl Stock
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of ESP32 Digitizer.
# You may redistribute and/or modify it under the GNU General Public License
# as published by the Free Software Foundation, either version 3 of the License,
# or (at your option) any later version.
#
# Distributed WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
# Public License in the repository LICENSE file or <https://www.gnu.org/licenses/>.

"""PC-only comparison tests: timing/scale recovery, held-out distortion and logs.

Run with Python's standard library; never opens serial ports or imports pyserial.
"""
import math
import unittest
from compareWaveforms import compare, parseInput, parseOutput


def signal(at):
    """Use an asymmetric, non-sinusoidal periodic test shape to constrain phase."""
    return 1300 + 400*math.sin(2*math.pi*at/100) + 90*math.sin(4*math.pi*at/100+0.3)


def traces(distort=False):
    """Known gain=0.002, offset=500, shift=+2ms; optional later output corruption."""
    inputs=[(iSample+0.4,signal(iSample+0.4)) for iSample in range(300)]
    outputs=[]
    for iSample in range(600):
        at=(iSample+0.5)*0.5
        value=(signal(at-2)-500)/0.002
        if distort and at>130:
            value+=60000*math.sin(2*math.pi*at/37)
        outputs.append((at,value))
    return inputs,outputs


class ComparisonChecks(unittest.TestCase):
    """Test actual analysis independently of hardware success/failure claims."""

    def testRecoverShiftAndScale(self):
        """A known pair must recover the explicit sign and units of the fit."""
        result=compare(*traces())
        self.assertAlmostEqual(result['shift'],2.0,delta=0.026)
        self.assertAlmostEqual(result['gain'],0.002,delta=0.000002)
        self.assertAlmostEqual(result['offset'],500,delta=0.5)
        self.assertLess(result['rms'],0.2)
        self.assertFalse(result['boundary'])

    def testHeldOutDistortionRemainsVisible(self):
        """Changing later data must not be hidden by refitting to those points."""
        baseline=compare(*traces())
        changed=compare(*traces(True))
        self.assertEqual(baseline['shift'],changed['shift'])
        self.assertAlmostEqual(baseline['gain'],changed['gain'])
        self.assertGreater(changed['rms'],50)

    def testFlatInputRejected(self):
        """A flat input has no meaningful amplitude or timing alignment."""
        _,outputs=traces()
        with self.assertRaises(ValueError):
            compare([(float(i),1000) for i in range(300)],outputs)

    def testInputProtocol(self):
        """Require complete IDs, ordered real timestamps and clean capture status."""
        lines=['INPUT_BEGIN run=12 count=300 clean=1 missed=0 maxGapUs=1000']
        lines += [f'INPUT,{i},{1000000+i*1000},1234' for i in range(300)]
        lines += ['INPUT_END run=12']
        text='\n'.join(lines)
        metadata,rows=parseInput(text)
        self.assertEqual(metadata[0],12)
        self.assertEqual(rows[-1],(299,1234))
        for altered in (text.replace('clean=1','clean=0'),text.replace('INPUT_END run=12','INPUT_END run=13'),
                        text.replace('INPUT,2,1002000,1234\n','')):
            with self.assertRaises(ValueError):
                parseInput(altered)

    def testOutputProtocol(self):
        """Reject incomplete bins, NaN means and inconsistent bin timing."""
        lines=['WAVE_BEGIN run=12 rate=1024000 bins=600 samplesPerBin=512 clean=1']
        lines += [f'WAVE,{i},-1,1,0.0' for i in range(600)]
        lines += ['WAVE_END run=12']
        text='\n'.join(lines)
        metadata,rows=parseOutput(text)
        self.assertEqual(metadata[0],12)
        self.assertEqual(rows[0][0],0.25)
        for altered in (text.replace('samplesPerBin=512','samplesPerBin=511'),
                        text.replace('WAVE,0,-1,1,0.0','WAVE,0,-1,1,nan'),
                        text.replace('WAVE,599,-1,1,0.0\n','')):
            with self.assertRaises(ValueError):
                parseOutput(altered)

    def testBurstProtocol(self):
        """Retain burst means while rejecting hidden NaNs and impossible extrema."""
        lines=['INPUT_BEGIN run=12 count=300 clean=1 missed=0 maxGapUs=1000 mode=burst']
        lines += [f'INPUT,{i},{1000250+i*1000},1234.5,1200,1300,20,505' for i in range(300)]
        lines += ['INPUT_END run=12']
        text='\n'.join(lines)
        metadata,rows=parseInput(text)
        self.assertEqual(metadata[5],'burst')
        self.assertEqual(rows[0],(0.25,1234.5))
        for altered in (text.replace('1234.5','nan'),text.replace('1200,1300','1250,1300'),
                        text.replace(',20,505',',0,505'),text.replace(',20,505',',20,0'),
                        text.replace(' mode=burst','')):
            with self.assertRaises(ValueError):
                parseInput(altered)


if __name__ == '__main__':
    unittest.main()
