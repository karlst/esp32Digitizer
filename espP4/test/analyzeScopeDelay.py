# Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
"""Decode a stopped four-channel scope export and compare sine-wave phases.

Reading path: main -> loadCapture -> decode -> fitFrequency/fitSine.

CH1 is the analog input, CH2 FSYNC, CH3 DCLK and CH4 DOUT0, all on
the same scope time axis.

Each FSYNC rising edge starts 8 status bits followed by 24 signed data bits.
Read on rising DCLK.

Four numbered sections in main follow
the method in docs/SINE-PHASE-DELAY-TEST.md. Before those steps, decode converts
the digital waveforms to signed ADC values and attaches their FSYNC times.
1. Estimate frequency: fitFrequency searches for the best fit to CH1. No FFT
   (Fast Fourier Transform) is used anywhere in this program.
2. Fit the input sine wave: fitSine uses least squares on recorded CH1 volts.
3. Fit the output sine wave: fitSine uses least squares on decoded ADC values.
   Both fits use the same frequency and the scope's original shared time axis.
4. Calculate delay: phaseDelay converts input phase minus output phase into ns.

Each fit returns ONE phase for all points supplied to it, not a phase per point.
The differences between points and the fitted curve are voltage/code residuals,
not individual phase measurements. The eight optional segment fits in main
produce separate delay estimates to examine variation within one capture.
No statistical uncertainty for a fitted phase is calculated by this program.

No GPIO6 timestamp, P4 timer, or earlier 2553-ns correction enters this calculation.
The ADC curve is plotted at output FSYNC times, not corrected analog-input times.
Thus a delayed ADC response appears as a phase lag relative to the CH1 input.
The analog curve and ADC curve need not have the same number of measured points;
each is fitted at its own measured times. No extra ADC samples are invented.
"""
import argparse
import collections
import gzip
import json
from pathlib import Path

import numpy as np


def loadCapture(directory):
    """Load an export directory for main; return seconds and four voltage arrays.

    Require a complete manifest, exactly CH1..CH4 and equal recorded time arrays.
    Compressed CSV files are loaded into PC memory; this is not a streaming or
    board-side operation. Missing/damaged files or differing grids raise errors.
    This loader does not recompute the manifest's raw-reply checksums or verify
    physical probe wiring; those are separate from the checks performed here.
    """
    manifest = json.loads((directory / 'manifest.json').read_text())
    if manifest['status'] != 'complete':
        raise ValueError('Export is incomplete')
    channels = {}
    times = None

    # Each CSV supplies its own time column. Compare the columns rather than
    # assuming that equal array lengths imply simultaneous sample positions.
    for channel in manifest['channels']:
        with gzip.open(directory / channel['csv'], 'rt') as stream:
            values = np.loadtxt(stream, delimiter=',', skiprows=1)
        if times is None:
            times = values[:, 0]
        elif not np.array_equal(times, values[:, 0]):
            raise ValueError('Channel time grids differ')
        channels[channel['channel']] = values[:, 1]
    if set(channels) != {1, 2, 3, 4}:
        raise ValueError('Need exactly CH1 through CH4')
    return times, channels


def risingEdges(times, values, threshold):
    """Interpolate threshold crossings; return times, without suppressing glitches.

    A short or extra clock interval must remain visible in the frame checks.
    Linear interpolation here locates a digital edge between scope points;
    it does not interpolate ADC voltages or assume an ADC transition shape.
    times is in seconds, values and threshold in volts. Return one interpolated
    time per low-to-high crossing. These are scope observations of a wire, not
    timestamps read from the P4 capture peripheral.
    """
    indices = np.flatnonzero((values[:-1] < threshold) & (values[1:] >= threshold))
    fractions = (threshold - values[indices]) / (values[indices + 1] - values[indices])
    return times[indices] + fractions * (times[indices + 1] - times[indices])


def decode(times, channels, threshold):
    """Sample FSYNC/data at rising clocks and check every complete 32-bit frame.

    Partial first/last frames are excluded. A sample's timestamp is its physical
    FSYNC rising crossing, not the later DCLK edge or completion of its word.
    The status mask is the same as FrameDecoder.cpp. Payload corruption can still
    evade status checks, so threshold sensitivity and sine residuals also matter.
    main calls this for three thresholds in volts. Return sample times in
    seconds, signed ADC codes, and counts of detected framing/status problems.
    A malformed frame is counted rather than silently repaired. main decides
    whether the central-threshold result is suitable for fitting.
    """
    # The scope recorded analog voltage even on digital wires. Locate each
    # rising DCLK edge, then interpolate the recorded FSYNC and DOUT0 voltages
    # at that instant. Compare with the threshold to obtain Boolean bit values.
    clocks = risingEdges(times, channels[3], threshold)
    fs = np.interp(clocks, times, channels[2]) >= threshold
    data = np.interp(clocks, times, channels[4]) >= threshold
    # A rising FSYNC level observed at a clock marks the first bit of a frame.
    # Keep the separate physical FSYNC crossings for timestamps: the first
    # DCLK edge and the FSYNC edge need not occur at exactly the same instant.
    starts = np.flatnonzero(fs[1:] & ~fs[:-1]) + 1
    fsEdges = risingEdges(times, channels[2], threshold)
    sampleTimes, codes = [], []
    headers = collections.Counter()
    badLengths = 0
    badStatus = 0

    # Two successive starts bracket a complete frame. Bits before the first
    # recognized start and after the final start are deliberately not decoded.
    for start, end in zip(starts[:-1], starts[1:]):
        if end - start != 32:
            badLengths += 1
        else:
            # Shift earlier bits toward the most significant end as later bits
            # arrive. The status mask matches FrameDecoder: only the selected
            # status bits are checked, not a checksum of the sample payload.
            word = 0
            for bit in data[start:end]:
                word = (word << 1) | int(bit)
            header = word >> 24
            headers[header] += 1
            if header & 0x5f != 0x08:
                badStatus += 1
            # Twenty-four-bit two's-complement values use bit 23 as the sign.
            # For example, 0xffffff represents -1, not positive 16,777,215.
            code = word & 0xffffff
            if code & 0x800000:
                code -= 0x1000000
            # Choose the last measured FSYNC rise at or before the first bit.
            # Attach the value to that output event, not to the last bit and
            # not to an assumed analog measurement time corrected for delay.
            edgeIndex = np.searchsorted(fsEdges, clocks[start], side='right') - 1
            if edgeIndex < 0:
                raise ValueError('No FSYNC edge preceding frame')
            sampleTimes.append(fsEdges[edgeIndex])
            codes.append(code)
    gaps = np.diff(clocks)
    checks = {'clockEdges': len(clocks), 'completeFrames': len(codes),
              'badFrameLengths': badLengths, 'badStatus': badStatus,
              'statusHistogram': {hex(k): v for k, v in headers.items()},
              'clockGapMinNs': float(gaps.min() * 1e9),
              'clockGapMaxNs': float(gaps.max() * 1e9)}
    return np.array(sampleTimes), np.array(codes), checks


def fitSine(times, values, frequency):
    """Fit one sine at a specified frequency and return its phase and fit quality.

    Called by fitFrequency to score candidate frequencies, and by main to fit
    the analog input and ADC output separately. times contains seconds on the
    scope's shared axis; values contains either CH1 volts or signed ADC codes.
    frequency is in Hz and is held fixed during this call.

    Offset, a and b are chosen to minimize the sum of squared differences between
    measured values and offset + a*sin(w*t) + b*cos(w*t), where w = 2*pi*f.
    This is a fit to all supplied points, not a selected edge or peak crossing.
    Every point has equal weight within its own fit.

    This shared helper implements steps 2 and 3. It is also used during step 1
    to compare candidate frequencies. For each supplied point, the fitted value
    is subtracted from the measured value. Those differences are squared and
    summed; np.linalg.lstsq selects the coefficients giving the smallest sum.
    One offset, one amplitude and one phase describe the whole supplied set.

    The identity A*sin(w*t + phi) = A*cos(phi)*sin(w*t)
    + A*sin(phi)*cos(w*t) gives a = A*cos(phi), b = A*sin(phi).
    Therefore A = sqrt(a*a + b*b), and phi = atan2(b, a), in radians.
    atan2 uses both signs to distinguish the four quadrants of a cycle.

    Independent amplitude and offset let volts and ADC codes be compared without
    converting codes to volts. Phase still uses the original shared time origin:
    shifting each curve's times independently would destroy the measured lag.

    Returns offset/amplitude in the units of values, phase in radians, residual
    RMS in the units of values, and residual RMS divided by fitted amplitude.
    Residual means measured value minus fitted value. Its RMS describes departure
    from a sine; it is not a phase confidence interval or an absolute timing bound.
    RMS means root mean square: square the residuals, average, then take the
    square root. The per-point residual array is not a list of phase shifts.
    The fit does not calculate the variance or confidence interval of its phase.
    Noise, distortion and short records can bias phase despite a successful fit.
    This function does not reject weak signals or ill-conditioned short fits.
    """

    # Each row represents one measurement time. The three columns say how much
    # the constant offset, sine and cosine contribute at that time. NumPy's
    # least-squares solver returns their three coefficients in this same order.
    # rcond=None uses NumPy's default cutoff for numerically dependent columns;
    # the extra solver outputs (including rank) are not checked here.
    angle = 2 * np.pi * frequency * times
    matrix = np.column_stack((np.ones(len(times)), np.sin(angle), np.cos(angle)))
    coefficients = np.linalg.lstsq(matrix, values, rcond=None)[0]

    # Reconstruct the fitted values with matrix multiplication, then measure
    # what remains unexplained. hypot calculates sqrt(a*a + b*b). The returned
    # phase describes the sine's position in its cycle at scope time zero.
    residual = values - matrix @ coefficients
    amplitude = np.hypot(coefficients[1], coefficients[2])
    
    return {'offset': float(coefficients[0]), 'amplitude': float(amplitude),
            'phase': float(np.arctan2(coefficients[2], coefficients[1])),
            'residualRms': float(np.sqrt(np.mean(residual ** 2))),
            'relativeResidualRms': float(np.sqrt(np.mean(residual ** 2)) / amplitude)}


def fitFrequency(times, values, nominal):
    """Estimate frequency in Hz from CH1 before main compares input/output phases.

    times is scope time in seconds, values is CH1 voltage, and nominal is the
    operator's --frequency setting in Hz. The generator setting restricts the
    search to +/-2%; it is not assumed to be the exact signal frequency.

    Step 1 uses repeated least-squares fits. At each candidate
    frequency, offset, amplitude and phase are fitted anew. The frequency is
    selected by comparing the remaining differences from the measured input.

    This search averages groups of 32 scope points to reduce computation
    and some high-frequency noise. At 3.2 ns per point, each group spans 102.4 ns.
    For an ideal sine on a uniform grid, assigning the average to the group's
    mean time preserves phase while changing amplitude. It does not remove
    all pickup or distortion. main fits the final phase using unaveraged CH1.

    Return the frequency with the lowest fitted residual in the selected local
    search interval. An optimum on the coarse grid's endpoint raises an error.
    This search does not prove a unique optimum or quantify frequency uncertainty.
    """

    # Reshape complete consecutive groups into rows, then average each row.
    # Discard at most 31 trailing points here; the final phase fit still uses
    # the original input points within the complete ADC sample time span.
    count = len(times) // 32 * 32
    fitTimes = times[:count].reshape(-1, 32).mean(axis=1)
    fitValues = values[:count].reshape(-1, 32).mean(axis=1)

    # First try 41 equally spaced frequencies. fitSine re-estimates offset and
    # amplitude at each frequency, so the score measures the remaining mismatch.
    grid = np.linspace(nominal * 0.98, nominal * 1.02, 41)
    scores = [fitSine(fitTimes, fitValues, f)['residualRms'] for f in grid]
    best = int(np.argmin(scores))
    if best in (0, len(grid) - 1):
        raise ValueError('Frequency optimum outside search range')
    
    # Refine between the neighbors of the best grid point. Each iteration tests
    # two interior frequencies and removes the less promising end of the interval
    # (golden-section search). Forty-five iterations narrow the numerical search;
    # that narrow interval is NOT a measurement confidence interval.
    lo, hi = grid[best - 1], grid[best + 1]
    for iteration in range(45):
        left = hi - (hi - lo) / 1.61803398875
        right = lo + (hi - lo) / 1.61803398875
        if fitSine(fitTimes, fitValues, left)['residualRms'] < fitSine(fitTimes, fitValues, right)['residualRms']:
            hi = right
        else:
            lo = left
    return (lo + hi) / 2


def phaseDelay(inputFit, outputFit, frequency):
    """Convert two fitted sine phases at one frequency into output lag in ns.

    main supplies fitSine results for the input voltage and decoded ADC values,
    both fitted at frequency Hz on the same scope time axis. If output is input
    delayed by tau seconds, its phase is phiOut = phiIn - 2*pi*f*tau.
    Hence tau = (phiIn - phiOut)/(2*pi*f). A positive result means output lags.

    Step 4 operates on just two fitted phase numbers: one for the input and one
    for the output. No per-sample delays are calculated or averaged here. One
    delay estimate is returned per pair of fits. Different estimates arise when
    another capture or another subset of points is fitted. Their variation must
    be examined separately; this conversion does not produce a variance or CI.

    A sine repeats every cycle: phase alone cannot distinguish tau from tau
    plus any whole number of periods. This function chooses the phase difference
    in [-pi, pi), giving a delay in [-half a period, half a period).
    Example: at 200 kHz, a 2524-ns lag is reported as approximately -2476 ns.
    Adding one 5000-ns period selects another equivalent phase branch. That
    selection requires evidence from other measurements; this function does not
    silently choose the branch closest to the earlier 2553-ns calibration.

    This is phase delay at one frequency. Group delay instead describes how
    phase changes with frequency; these quantities need not be equal. Neither
    phase wrapping nor the conversion below calculates uncertainty or removes
    probe skew, loading, or other systematic errors.
    """
    # Modulo removes whole cycles after shifting the allowed interval by pi.
    # Divide radians by radians/second, then convert seconds to nanoseconds.
    phase = (inputFit['phase'] - outputFit['phase'] + np.pi) % (2 * np.pi) - np.pi
    return float(phase / (2 * np.pi * frequency) * 1e9)


def writePlot(directory, times, analog, sampleTimes, codes, inputFit, outputFit, frequency, delayNs):
    """Save an SVG comparison over two cycles, using independently normalized signals.

    Gray is the measured scope input averaged in 16-point groups (51.2 ns when
    scope spacing is 3.2 ns); blue dots are decoded ADC samples at their FSYNC times. Smooth
    lines are fitted sinusoids, not additional measured values. This makes the
    phase displacement visible without confusing volts with digital logic levels.
    """
    stop = 2 / frequency
    count = len(times) // 16 * 16
    meanTimes = times[:count].reshape(-1, 16).mean(axis=1)
    meanValues = analog[:count].reshape(-1, 16).mean(axis=1)
    analogMask = (meanTimes >= 0) & (meanTimes <= stop)
    adcMask = (sampleTimes >= 0) & (sampleTimes <= stop)
    x = lambda t: 90 + t / stop * 1040
    y = lambda v: 270 - v * 140
    elements = ['<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="530" viewBox="0 0 1200 530">',
                '<rect width="1200" height="530" fill="white"/>',
                '<g font-family="Arial,sans-serif" font-size="16" fill="#222">',
                '<text x="70" y="35" font-size="24">Independent scope measurement: input versus decoded ADC output</text>',
                f'<text x="70" y="65">Sine phase delay: {delayNs:.1f} ns at {frequency:.3f} Hz — preliminary, not an absolute 95% interval</text>']
    for tick in np.linspace(0, stop, 9):
        elements.append(f'<path d="M{x(tick):.2f},100 V430" stroke="#ddd"/>')
        elements.append(f'<text x="{x(tick)-15:.2f}" y="455">{tick*1e6:.0f}</text>')
    for level in (-1, 0, 1):
        elements.append(f'<path d="M90,{y(level)} H1130" stroke="#ddd"/>')
        elements.append(f'<text x="45" y="{y(level)+5}">{level}</text>')
    points = ' '.join(f'{x(t):.2f},{y((v-inputFit["offset"])/inputFit["amplitude"]):.2f}'
                      for t, v in zip(meanTimes[analogMask], meanValues[analogMask]))
    elements.append(f'<polyline points="{points}" fill="none" stroke="#aab0b8" stroke-width="1"/>')
    for fit, color in ((inputFit, '#15803d'), (outputFit, '#2563eb')):
        grid = np.linspace(0, stop, 1000)
        points = ' '.join(f'{x(t):.2f},{y(np.sin(2*np.pi*frequency*t+fit["phase"])):.2f}' for t in grid)
        elements.append(f'<polyline points="{points}" fill="none" stroke="{color}" stroke-width="2"/>')
    for t, value in zip(sampleTimes[adcMask], codes[adcMask]):
        elements.append(f'<circle cx="{x(t):.2f}" cy="{y((value-outputFit["offset"])/outputFit["amplitude"]):.2f}" r="2" fill="#2563eb"/>')
    elements += ['<text x="450" y="480">Time on the scope’s shared axis (µs)</text>',
                 '<text x="70" y="510">Normalized amplitude: gray = measured input; green = input fit; blue = ADC samples and fit.</text>',
                 '</g></svg>']
    (directory / 'sineComparison.svg').write_text('\n'.join(elements), encoding='utf-8')


def main():
    """Analyze one saved capture, retain checks even on failure, and print results.

    Threshold sweeps expose unstable digital decoding. Eight segment estimates
    describe variation within one acquisition. They share its fitted frequency;
    independence is not established, and they are not automatically eight
    independent experimental repeats or a 95% interval for absolute delay.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--frequency', type=float, default=10000)
    args = parser.parse_args()
    times, channels = loadCapture(args.capture)
    report = {'capture': str(args.capture), 'status': 'decode checks failed', 'thresholdChecks': {}}
    decoded = {}

    # Preparation: recover ADC values and their output FSYNC times from the
    # saved digital voltage arrays. This is separate from fitting sine waves.
    # codes is the array of signed 24-bit ADC readings reconstructed from DOUT0
    for threshold in (0.7, 0.9, 1.1):
        sampleTimes, codes, checks = decode(times, channels, threshold)
        report['thresholdChecks'][str(threshold)] = checks
        decoded[threshold] = (sampleTimes, codes)
    
    checks = report['thresholdChecks']['0.9']
    
    if checks['badFrameLengths'] == 0 and checks['badStatus'] == 0 and checks['completeFrames'] > 100:
        # The central 0.9-V decoding must have no detected framing/status errors
        # and enough samples before fitting.

        # 1) ESTIMATE FREQUENCY FROM THE INPUT
        # Search near the generator setting using CH1 only. Score each trial frequency
        # by least squares.
        frequency = fitFrequency(times, channels[1], args.frequency)
        sampleTimes, codes = decoded[0.9]

        # 2) USE LEAST SQUARES TO FIT THE INPUT SINE WAVE
        # Input uses the original CH1 voltages; output uses signed ADC codes at output FSYNC times.

        # exclude times outside the sample interval
        mask = (times >= sampleTimes[0]) & (times <= sampleTimes[-1])

        #perform least squares fit
        inputFit = fitSine(times[mask], channels[1][mask], frequency)

        # 3) USE LEAST SQUARES TO FIT THE OUTPUT SINE WAVE
        # Fit the signed ADC values at their FSYNC times. Converting codes to
        # volts is unnecessary: amplitude and offset are fitted independently.
        outputFit = fitSine(sampleTimes, codes, frequency)

        # 4) CALCULATE DELAY FROM THE PHASE DIFFERENCE
        # Subtract the two fitted phases, express the result within +/- half
        # a cycle, divide by 2*pi*frequency, and convert seconds to nanoseconds.
        # This gives one delay estimate, not a distribution of sample delays.
        report.update(status='phase estimate; absolute uncertainty not established',
                      frequencyHz=frequency, inputFit=inputFit, outputFit=outputFit,
                      delayNs=phaseDelay(inputFit, outputFit, frequency))

        # ADDITIONAL CHECK: SENSITIVITY TO DIGITAL VOLTAGE THRESHOLD
        # Repeat the digital edge interpretation at 0.7, 0.9 and 1.1 V. The analog
        # input fit stays fixed. Compare decoded codes as well as delay: matching
        # codes support stable bit decoding, while changing FSYNC crossing times
        # can still shift the measured delay. This is not an ADC-voltage threshold.
        report['thresholdSensitivity'] = {}
        
        for threshold, (otherTimes, otherCodes) in decoded.items():
            otherChecks = report['thresholdChecks'][str(threshold)]
            report['thresholdSensitivity'][str(threshold)] = {
                'identicalCodes': bool(np.array_equal(codes, otherCodes)),
                'delayNs': phaseDelay(inputFit, fitSine(otherTimes, otherCodes, frequency), frequency)
                if otherChecks['badFrameLengths'] == 0 and otherChecks['badStatus'] == 0 else None}

        # ADDITIONAL CHECK: VARIATION BETWEEN EIGHT PARTS OF THIS CAPTURE
        # Divide the complete-sample span into eight non-overlapping time ranges.
        # Each range includes its left endpoint and excludes its right endpoint;
        # the last ADC sample is therefore omitted from these segment fits only.
        segments = []
        limits = np.linspace(sampleTimes[0], sampleTimes[-1], 9)
        
        for lo, hi in zip(limits[:-1], limits[1:]):
            analogMask = (times >= lo) & (times < hi)
            adcMask = (sampleTimes >= lo) & (sampleTimes < hi)
            segments.append(phaseDelay(fitSine(times[analogMask], channels[1][analogMask], frequency),
                                       fitSine(sampleTimes[adcMask], codes[adcMask], frequency), frequency))
        report['segmentDelaysNs'] = segments
        report['limitations'] = [
            f'One {len(times) * (times[1] - times[0]) * 1000:g}-ms capture; '
            'segment variation is not an absolute 95% confidence interval.',
            'Phase delay at one frequency, modulo one sine period; not yet a group-delay measurement.',
            'FSYNC is measured directly at the ADC; the voltage translator is excluded.',
            'Probe/channel skew, analog-path loading, pickup and waveform distortion remain systematic uncertainties.',
            'Normalizing amplitude/offset does not correct analog probe phase error.'
        ]
        writePlot(args.capture, times, channels[1], sampleTimes, codes,
                  inputFit, outputFit, frequency, report['delayNs'])
        np.savetxt(args.capture / 'decodedSamples.csv', np.column_stack((sampleTimes, codes)),
                   delimiter=',', header='fsyncTime_s,signedAdcCode', comments='', fmt=['%.12g', '%d'])
    (args.capture / 'delayAnalysis.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
