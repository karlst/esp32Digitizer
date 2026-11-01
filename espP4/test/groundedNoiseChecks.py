# Copyright (C) 2026 Karl Stock
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the noise-analysis code with made-up readings whose properties are known.

Run this file on the PC to exercise the Python calculations and report reader.
GroundedNoiseChecks tests noise levels, inserted frequencies, damaged transfers
and saved graphs. It creates repeatable data, not measurements from a board.

With --firmware-report and an executable path, run the compiled PC version of
the C++ readback reporter instead. checkFirmwareReport checks every emitted
reading against the known input pattern. Neither mode opens serial ports or
operates hardware. Passing these checks does not establish real ADC performance.
"""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zlib
import numpy as np
from externalGeneratorChecks import externalFixture
from hardwareSmokeRetainedReport import analyzeRetained
from groundedNoiseReport import (readSamples, calculateNoise, findPeaks, analyzeNoise,
                                 microvoltsPerCode, sampleRate)


def makeTransfer(codes):
    """Return simulated P4 serial text containing the supplied signed ADC numbers.

    Tests pass a one-second codes array. Pack its numbers into three bytes each,
    lowest byte first, and emit the same header, 64-reading rows and completion line
    as the P4. Python's CRC-32 checksum covers the original bytes. This packs data
    independently of the C++ reporter so the reader is checked against another method.
    """

    # Represent each number as four bytes, lowest first (NumPy dtype <i4),
    # then keep only the three bytes stored by the ADC recording format.
    words = np.asarray(codes, dtype=np.int32).astype('<i4').view(np.uint8).reshape(-1, 4)
    raw = words[:, :3].copy().tobytes()

    # Position is a count of readings, not bytes. Encode each byte as two hex
    # characters; the completion checksum still covers bytes, not those characters.
    rows = [f'NOISE_BEGIN version=1 firstSample={sampleRate} count={sampleRate} rate={sampleRate}']
    for offset in range(0, len(raw), 64 * 3):
        rows.append(f'NOISE_DATA,{offset // 3},{raw[offset:offset + 64 * 3].hex()}')
    rows.append(f'NOISE_END count={sampleRate} crc={zlib.crc32(raw):08x}')
    return '\n'.join(rows)


class GroundedNoiseChecks(unittest.TestCase):
    """Check known noise levels, inserted frequencies and rejection of damaged data.

    Python's unittest runner calls each test method. Assertions compare the calculated
    answers with expected values; a failed assertion makes the test suite fail.
    """

    @classmethod
    def setUpClass(cls):
        """Create one repeatable noise sequence for this class's tests to reuse.

        unittest calls this once before the methods. Seed 731 starts the random-number
        generator in the same state each time, so a failure can be reproduced. Request
        a normal distribution with standard deviation 20 uV, then round to integer ADC
        numbers. A finite random sequence will be close to 20 uV, not exactly 20 uV.
        """
        cls._codes = np.rint(np.random.default_rng(731).normal(0, 20 / microvoltsPerCode, sampleRate)).astype(np.int32)
        cls._transfer = makeTransfer(cls._codes)

    def testSignedBytes(self):
        """Check that positive and negative readings keep their values and order.

        Include zero, small values and values near the ADC limits. Use numbers just inside
        the limits here; testClipping separately checks rejection at the positive limit.
        """
        codes = np.resize(np.array([-8388607, -65536, -1, 0, 1, 65536, 8388606]), sampleRate)
        decoded, raw = readSamples(makeTransfer(codes), sampleRate)
        np.testing.assert_array_equal(decoded, codes)
        self.assertEqual(len(raw), sampleRate * 3)

    def testDamagedTransfer(self):
        """Change valid transfer text and require the reader to reject each damaged copy.

        Cover a changed digit, a missing row, a repeated row, a missing completion line,
        a repeated completion line, an incorrect row position and an incorrect starting
        sample position. Each case must raise ValueError instead of returning readings.
        """
        lines = self._transfer.splitlines()
        corrupt = lines.copy()
        corrupt[1] = corrupt[1][:-1] + ('1' if corrupt[1][-1] != '1' else '2')
        variants = ['\n'.join(corrupt), '\n'.join(lines[:2] + lines[3:]),
                    '\n'.join(lines[:2] + lines[1:]), '\n'.join(lines[:-1]),
                    self._transfer + '\n' + lines[-1],
                    self._transfer.replace('NOISE_DATA,64,', 'NOISE_DATA,65,'),
                    self._transfer.replace('firstSample=1024000', 'firstSample=0')]
        for iVariant, text in enumerate(variants):
            with self.subTest(variant=iVariant), self.assertRaises(ValueError):
                readSamples(text, sampleRate)

    def testClipping(self):
        """Reject a reading at the ADC's largest representable positive value.

        The transfer has a valid checksum, so the failure must concern the reading itself.
        At that limit, the true input might be larger than the ADC can represent.
        """
        codes = self._codes.copy()
        codes[100] = 8388607
        with self.assertRaises(ValueError):
            readSamples(makeTransfer(codes), sampleRate)

    def testKnownSineAndOffset(self):
        """Check a known 60 Hz sine wave before and after adding a constant voltage.

        The sine has 10 uV RMS; its peak amplitude is 10 times the square root of two.
        The FFT should recover 10 uV RMS after correcting for the Hann window. Adding
        1200 uV must change the mean, not the variation or frequency. The standard
        deviation expectation includes the N-1 adjustment used by calculateNoise.
        """
        rate = 4096
        signal = 10 * np.sqrt(2) * np.sin(2 * np.pi * 60 * np.arange(rate) / rate)
        for offset in (0, 1200):
            stats, frequencies, power, values = calculateNoise((signal + offset) / microvoltsPerCode, rate)
            self.assertAlmostEqual(stats['stddevUv'], 10 * np.sqrt(rate / (rate - 1)), places=8)
            self.assertAlmostEqual(stats['spectrumRmsUv'], 10, places=8)
            self.assertAlmostEqual(stats['meanUv'], offset, places=8)
            self.assertEqual(frequencies[np.argmax(power)], 60)
            self.assertAlmostEqual(stats['windowBandwidthHz'], 1.5)

    def testTwoTones(self):
        """Find 60 Hz and 100 kHz after adding those sine waves to the noise sequence.

        The added waves have peak amplitudes of 10 and 5 uV. Both frequencies must appear
        in the reported peak list; this test does not check their measured amplitudes.
        """
        time = np.arange(sampleRate) / sampleRate
        signal = self._codes * microvoltsPerCode + 10 * np.sin(2 * np.pi * 60 * time) + 5 * np.sin(2 * np.pi * 100000 * time)
        stats, frequencies, power, values = calculateNoise(signal / microvoltsPerCode)
        peaks = findPeaks(frequencies, power)
        self.assertIn(60, [peak['frequencyHz'] for peak in peaks])
        self.assertIn(100000, [peak['frequencyHz'] for peak in peaks])

    def testTargetAndFlatLine(self):
        """Check that about 20 uV passes, about 30 uV fails, and no variation fails.

        Scale the same readings to exercise the 25 uV limit. Allow a small tolerance when
        checking the measured spread because the finite sequence and rounding change it
        slightly. A constant value cannot demonstrate working acquisition.
        """
        for multiplier, expected in ((1, True), (1.5, False), (0, False)):
            with self.subTest(multiplier=multiplier):
                text = makeTransfer(np.rint(self._codes * multiplier).astype(np.int32))
                result = analyzeNoise(text, {'checks': []}, 0)
                self.assertEqual(result['passed'], expected)
                self.assertAlmostEqual(result['noise']['stddevUv'], multiplier * 20, delta=.1)

    def testSharedStorageChecks(self):
        """Require valid recording reports as well as acceptable noise measurements.

        Reuse simulated Feather/P4 reports and add the noise transfer. First require the
        combined report to pass. Then change one field at a time to report overflow,
        dropped readings, failed completion, old firmware or disabled noise reporting.
        Each changed report must be rejected before it can produce a noise PASS.
        """
        feather, p4 = externalFixture()
        p4 = p4.replace('SMOKE_P4 version=7', 'SMOKE_P4 version=9') + '\nNOISE OK enabled=1\n' + self._transfer
        result = analyzeRetained(feather, p4, noise=True)
        self.assertTrue(result['passed'])
        for before, after in [('overflow=0', 'overflow=1'), ('drops=0', 'drops=1'),
                              ('clean=1', 'clean=0'), ('version=9', 'version=8'),
                              ('NOISE OK enabled=1', 'NOISE OK enabled=0')]:
            with self.subTest(before=before), self.assertRaises(ValueError):
                analyzeRetained(feather, p4.replace(before, after), noise=True)

    def testSavedReport(self):
        """Keep valid measurements available for inspection when their noise is too large.

        Double the noise to exceed the limit, write into a temporary directory, and check
        for four graphs, the FAIL text and the complete raw-data file. The temporary
        directory is removed automatically. This does not check the graphs' appearance.
        """
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'comparison.html'
            result = analyzeNoise(makeTransfer(self._codes * 2), {'checks': []}, 0, path)
            self.assertFalse(result['passed'])
            page = path.read_text(encoding='utf-8')
            self.assertEqual(page.count('<svg'), 4)
            self.assertIn('FAIL', page)
            self.assertIn('not proof', page)
            self.assertEqual(path.with_name('noise-samples.bin').stat().st_size, sampleRate * 3)

    def testExistingWaveformsOnNewFirmware(self):
        """Keep the earlier square-wave analyses working with the version-9 P4 identity.

        Supply simulated 0.5 Hz and 1 kHz reports with the updated version number. Both
        must still pass their existing generator comparisons; no real generator is used.
        """
        from fastSquareChecks import fastFixture
        for frequency, fixture in ((0.5, externalFixture), (1000, fastFixture)):
            with self.subTest(frequency=frequency):
                feather, p4 = fixture()
                p4 = p4.replace('SMOKE_P4 version=7', 'SMOKE_P4 version=9').replace('SMOKE_P4 version=8', 'SMOKE_P4 version=9')
                result = analyzeRetained(feather, p4, scopeLevels=(1.355, 1.855), frequencyHz=frequency)
                self.assertTrue(result['passed'])


def checkFirmwareReport(executable):
    """Run the PC build of the C++ reporter and check every reading it prints.

    executable is the path supplied by the PowerShell PC-test runner. Its
    --noise-fixture mode feeds three seconds of known numbers through the real
    readback parser and prints only the middle second. Capture its standard output
    as text, check the transfer checksum, and compare every decoded number with the
    expected pattern. subprocess.run raises on a failed process or a 60-second
    timeout; assertions reject wrong readings. Print a success message and return
    no value when all checks pass. No firmware is uploaded and no board is accessed.
    """
    completed = subprocess.run([executable, '--noise-fixture'], check=True, capture_output=True, text=True, timeout=60)
    codes, raw = readSamples(completed.stdout, sampleRate)
    expected = np.array([-8388607, -123456, -1, 0, 1, 123456, 8388606])

    # The C++ pattern starts at recording sample zero, but output starts one
    # second later. Shift the repeating pattern by that many readings.
    np.testing.assert_array_equal(codes, expected[(np.arange(sampleRate) + sampleRate) % len(expected)])
    print('PASS: real C++ noise transfer, signed values, sample selection, split bytes and CRC')


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--firmware-report':
        checkFirmwareReport(sys.argv[2])
    else:
        unittest.main()
