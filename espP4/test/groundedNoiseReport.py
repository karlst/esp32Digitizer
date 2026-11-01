# Copyright (C) 2026 Karl Stock
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure noise in one second of grounded CH0 readings read back from SD.

The hardware runner records normally, saves the board logs, then calls
hardwareSmokeRetainedReport.analyzeRetained to check recording and storage errors.
That function calls analyzeNoise here with the P4 log and the checked results.
readSamples decodes the readings; calculateNoise measures their spread and uses
NumPy (Python's array-math library) to calculate the FFT. An FFT separates the
variation in a sequence of readings into contributions at different frequencies.
findPeaks looks for frequencies that stand above their neighbors. writePlot saves
the graphs, and summarize supplies the text printed by the runner.

All this work happens on the PC after recording. Statistics and FFT use individual
readings, without averaging them first. Grounding is the operator's responsibility;
the data checks cannot establish how the inputs were physically connected.
"""
import html
import re
import zlib
import numpy as np

# Match the board configuration: signed 24-bit readings use 2^23 counts for
# the positive full-scale voltage. The 25 uV limit is our chosen system target.
sampleRate = 1024000
referenceVolts = 2.5
microvoltsPerCode = referenceVolts * 1e6 / 8388608
targetMicrovolts = 25.0
peakThresholdDb = 20.0


def readSamples(text, firstSample):
    """Decode the P4's noise-transfer lines and check that the transfer is complete.

    analyzeNoise supplies text, the complete P4 log, and firstSample, the expected
    zero-based reading position measured from the beginning of the recording.
    Ignore unrelated log lines. Require one header, consecutive data rows and one
    completion line. Each row contains up to 64 readings, each represented by three
    bytes in hexadecimal text, with the lowest byte first.

    Compare the final CRC-32 checksum, a number calculated from all transferred
    bytes, with the P4's checksum. A mismatch indicates damaged data. Count and
    position checks also catch missing or repeated rows. Raise ValueError if these
    checks fail or a reading equals an ADC limit; a limit reading may be clipped.
    Return a NumPy array of signed ADC numbers and an unchanged copy of their bytes.
    These checks concern the transfer; the caller checks recording integrity.
    """

    # Accumulate the original bytes in row order. The row offset counts readings,
    # while raw holds three bytes per reading. No voltage conversion happens here.
    raw = bytearray()
    state = 'begin'
    for line in text.splitlines():
        if line.startswith(('NOISE_BEGIN', 'NOISE_DATA', 'NOISE_END')):
            if state == 'begin':
                expected = f'NOISE_BEGIN version=1 firstSample={firstSample} count={sampleRate} rate={sampleRate}'
                if line != expected:
                    raise ValueError('Noise transfer has a missing or incorrect header')
                state = 'data'
            elif state == 'data' and line.startswith('NOISE_DATA,'):
                match = re.fullmatch(r'NOISE_DATA,(\d+),([0-9a-fA-F]+)', line)
                if match is None:
                    raise ValueError('Malformed noise transfer row')
                count = len(raw) // 3
                expectedBytes = min(64, sampleRate - count) * 3
                if int(match[1]) != count or expectedBytes <= 0 or len(match[2]) != expectedBytes * 2:
                    raise ValueError('Noise transfer rows are missing, repeated or out of order')
                raw.extend(bytes.fromhex(match[2]))
            elif state == 'data':
                match = re.fullmatch(r'NOISE_END count=(\d+) crc=([0-9a-fA-F]{8})', line)
                if (match is None or int(match[1]) != sampleRate or len(raw) != sampleRate * 3 or
                        int(match[2], 16) != zlib.crc32(raw)):
                    raise ValueError('Noise transfer length or checksum failed')
                state = 'done'
            else:
                raise ValueError('Repeated noise transfer')
    if state != 'done':
        raise ValueError('Noise transfer is incomplete')

    # Group bytes in threes, then widen them to 32 bits before shifting them.
    # For example, FF FF FF first forms 16777215; subtracting 2^24 gives -1.
    # Shifting while still using 8-bit numbers would discard the upper bytes.
    parts = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
    codes = parts[:, 0] | (parts[:, 1] << 8) | (parts[:, 2] << 16)
    codes[codes >= 8388608] -= 16777216
    if np.any((codes == -8388608) | (codes == 8388607)):
        raise ValueError('Noise readings reach an ADC limit; check wiring and configuration')
    return codes, bytes(raw)


def calculateNoise(codes, rate=sampleRate):
    """Calculate voltage statistics and the noise contribution at each FFT frequency.

    analyzeNoise supplies codes, the signed ADC numbers, and rate, readings per second.
    Convert codes to microvolts using the nominal reference voltage. Statistics use
    every reading. Subtract the mean only for the FFT so a constant voltage offset
    does not dominate the frequency graph.

    The Hann window is a set of weights that reduces readings near the two ends of
    the interval toward zero. This reduces the artificial jump where an FFT treats
    the interval's end as joining its beginning. It changes the FFT, not the raw
    readings or their standard deviation.

    Return four objects: a dictionary of statistics; an array of frequencies in Hz;
    an equally sized array of noise power per Hz in microvolts squared per Hz; and
    the original readings converted to microvolts. One FFT entry is called a bin.
    With a one-second interval, these bins are spaced 1 Hz apart. Square-rooting
    power gives the graph's noise density in microvolts per square root of Hz.
    No FFT results are averaged. Raise ValueError for too few readings, non-finite
    values or a nonpositive sample rate.
    """
    values = np.asarray(codes, dtype=np.float64) * microvoltsPerCode
    if values.size < 8 or not np.all(np.isfinite(values)) or rate <= 0:
        raise ValueError('Noise analysis needs finite readings and a positive sample rate')

    # Keep the measured mean as a separate result. Center only the copy used
    # by the FFT; values still contains the actual voltages relative to ground.
    mean = float(np.mean(values))
    centered = values - mean
    window = .5 - .5 * np.cos(2 * np.pi * np.arange(values.size) / values.size)
    transform = np.fft.rfft(centered * window)

    # rfft returns zero and positive frequencies only. For real-valued readings,
    # the omitted negative frequencies carry matching power. Double the interior
    # bins to include it, but not zero or the endpoint at half the sample rate
    # (the Nyquist frequency). An odd-length record has no exact half-rate bin.
    # Dividing by rate * sum(window squared) compensates for the Hann weights
    # and expresses power per Hz, rather than power per displayed bin.
    power = np.abs(transform)**2 / (rate * np.sum(window**2))
    if values.size % 2 == 0:
        power[1:-1] *= 2
    else:
        power[1:] *= 2
    frequencies = np.fft.rfftfreq(values.size, 1 / rate)

    # ddof=1 uses N-1 in the standard-deviation calculation, where N is the
    # reading count. Sum power * bin spacing before taking the square root to
    # obtain RMS noise from the FFT. windowBandwidthHz describes the window
    # weighting: 1.5 Hz for this one-second record, versus 1 Hz bin spacing.
    statistics = dict(meanUv=mean, stddevUv=float(np.std(values, ddof=1)),
                      minimumUv=float(values.min()), maximumUv=float(values.max()),
                      peakToPeakUv=float(np.ptp(values)), count=int(values.size),
                      rate=rate, seconds=values.size / rate, spacingHz=rate / values.size,
                      windowBandwidthHz=float(rate * np.sum(window**2) / np.sum(window)**2),
                      spectrumRmsUv=float(np.sqrt(np.sum(power) * rate / values.size)))
    return statistics, frequencies, power, values


def findPeaks(frequencies, power):
    """Find narrow peaks that stand well above nearby noise in the FFT.

    analyzeNoise supplies the frequency and power arrays returned by calculateNoise.
    Check the strongest 300 points that are higher than the point immediately before
    them and at least as high as the point after them. Compare each with the median
    (middle value) of nearby power values. Look up to 50 bins on each side, excluding
    the peak and three bins on either side so the peak does not raise its own baseline.

    A 20 dB power difference means the peak is 100 times the nearby median power.
    Return up to ten dictionaries containing frequencyHz and aboveNearbyDb. Keep
    only the strongest selected peak within three bin spacings. This reports which
    frequencies deserve inspection, not their causes or their total RMS contributions.
    An empty list does not prove randomness; broad or weaker interference can remain.
    """

    # Locate candidate peaks, then inspect the strongest first so a weaker
    # neighboring point cannot displace the main peak.
    candidates = np.flatnonzero((power[1:-1] > power[:-2]) & (power[1:-1] >= power[2:])) + 1
    candidates = candidates[np.argsort(power[candidates])[-300:]][::-1]
    peaks = []
    for iBin in candidates:
        neighbors = np.concatenate((power[max(1, iBin - 50):max(1, iBin - 3)],
                                    power[iBin + 4:min(len(power), iBin + 51)]))
        if len(neighbors) >= 8 and power[iBin] > 0:

            # Require enough nearby points to estimate background power. The
            # tiny lower bound prevents division by zero for ideal test signals.
            floor = max(float(np.median(neighbors)), float(power.max()) * 1e-20)
            contrast = float(10 * np.log10(power[iBin] / floor))
            separated = all(abs(float(frequencies[iBin]) - peak['frequencyHz']) > 3 * (frequencies[1] - frequencies[0]) for peak in peaks)
            if contrast >= peakThresholdDb and separated and len(peaks) < 10:
                peaks.append(dict(frequencyHz=float(frequencies[iBin]), aboveNearbyDb=contrast))
    return peaks


def analyzeNoise(text, result, first, plotPath=None):
    """Add noise measurements to the checked recording results and optionally save graphs.

    analyzeRetained calls this after checking receiver, buffer and SD reports. text
    is the P4 log; result is the dictionary of those checked results. first is the
    zero-based position of the first reading retained on SD. The transferred second
    starts one second after that position. plotPath, when supplied, names the HTML
    file to write; save noise-samples.bin in the same directory.

    Update and return the same result dictionary. PASS requires a standard deviation
    greater than zero and at most 25 microvolts. A perfectly fixed value cannot show
    that acquisition was working, so it fails. Record mean offset and frequency peaks
    separately; neither changes this magnitude verdict. There is no offset acceptance
    limit. Save valid data and graphs even when the magnitude target fails. A damaged
    transfer raises ValueError before analysis or output files are written.
    """
    codes, raw = readSamples(text, first + sampleRate)
    stats, frequencies, power, values = calculateNoise(codes)
    peaks = findPeaks(frequencies, power)
    result.update(groundedNoise=True, calibration='NOMINAL 2.5 V ADC reference',
                  noise=stats, targetUv=targetMicrovolts, peaks=peaks,
                  passed=0 < stats['stddevUv'] <= targetMicrovolts,
                  analyzedFirstSample=first + sampleRate,
                  rawFormat='signed 24-bit, three bytes per reading, lowest byte first',
                  referenceVolts=referenceVolts, peakThresholdDb=peakThresholdDb)

    # Keep original bytes for later calculations without another hardware run.
    if plotPath is not None:
        plotPath.with_name('noise-samples.bin').write_bytes(raw)
        writePlot(result, frequencies, power, values, plotPath)
    return result


def summarize(result, includeChecks=True):
    """Return readable text containing the noise measurements and pass/fail result.

    The hardware runner and writePlot supply the dictionary returned by analyzeNoise.
    includeChecks includes the earlier recording and SD checks; the runner disables
    it when those checks have already been printed. The returned text separates the
    standard-deviation verdict from FFT peaks that still need interpretation.
    """
    stats = result['noise']
    checks = '\n'.join(result['checks']) + '\n' if includeChecks else ''
    verdict = 'PASS' if result['passed'] else 'FAIL'
    peaks = result['peaks']
    peakText = '; '.join(f"{peak['frequencyHz']:g} Hz ({peak['aboveNearbyDb']:.1f} dB above nearby spectrum)" for peak in peaks)
    if not peaks:
        peakText = 'No narrow peaks exceeded the 20 dB screening threshold; this does not prove randomness.'
    return (checks + f"Grounded-input noise: {stats['count']:,} individual readings, {stats['seconds']:.3f} s\n"
            f"{verdict}: standard deviation {stats['stddevUv']:.3f} uV; target <= {result['targetUv']:g} uV (nonzero spread required)\n"
            f"Mean offset: {stats['meanUv']:+.3f} uV\n"
            f"Minimum / maximum: {stats['minimumUv']:+.3f} / {stats['maximumUv']:+.3f} uV; peak-to-peak {stats['peakToPeakUv']:.3f} uV\n"
            f"FFT candidates: {peakText}\n"
            'Inspect the spectrum for repeating interference. PASS above applies only to the noise magnitude.\n'
            'Values assume a 2.5 V reference. This measures the connected board while SD writing is active, not the ADC chip alone.')


def svgPlot(title, x, y, xLabel, yLabel, logarithmicX=False, logarithmicY=False):
    """Return a graph as SVG markup for embedding in the HTML report.

    writePlot supplies the title, equally sized x/y arrays of selected graph points,
    and axis labels containing their units. Logarithmic flags select axes where equal
    distances represent equal multiplication, such as 1, 10 and 100. Draw six labeled
    grid positions on each axis. The caller chooses which readings to show; this
    function places those points on the page and joins them with a line.
    """
    x = np.asarray(x, dtype=float)
    y = np.asarray(y, dtype=float)

    # A logarithm cannot represent zero or a negative number. For drawing only,
    # replace such values with small positive bounds before taking logarithms.
    if logarithmicX:
        x = np.log10(np.maximum(x, 1e-12))
    if logarithmicY:
        y = np.log10(np.maximum(y, 1e-9))

    # Give constant-valued data a nonzero axis range and add vertical margin.
    xMin, xMax = float(x.min()), float(x.max())
    yMin, yMax = float(y.min()), float(y.max())
    if xMin == xMax:
        xMax = xMin + 1
    if yMin == yMax:
        yMin, yMax = yMin - 1, yMax + 1
    padding = (yMax - yMin) * .05
    yMin, yMax = yMin - padding, yMax + padding

    # Convert data coordinates to SVG page coordinates. The graph is 800 units
    # wide and 210 high; SVG y increases downward, hence the subtraction from 260.
    points = ' '.join(f'{75 + (a-xMin)/(xMax-xMin)*800:.2f},{260-(b-yMin)/(yMax-yMin)*210:.2f}' for a, b in zip(x, y))
    marks = []
    for iTick in range(6):
        fraction = iTick / 5
        a, b = xMin + fraction * (xMax - xMin), yMin + fraction * (yMax - yMin)
        xValue, yValue = (10**a if logarithmicX else a), (10**b if logarithmicY else b)
        marks.append(f'<path d="M {75+fraction*800} 50 V260 M75 {260-fraction*210} H875" stroke="#ddd"/>'
                     f'<text x="{75+fraction*800}" y="280" text-anchor="middle">{xValue:.3g}</text>'
                     f'<text x="68" y="{264-fraction*210}" text-anchor="end">{yValue:.3g}</text>')
    return (f'<h2>{html.escape(title)}</h2><svg viewBox="0 0 920 325" role="img" aria-label="{html.escape(title)}">'
            + ''.join(marks) + f'<polyline points="{points}" fill="none" stroke="#176b9b" stroke-width="1"/>'
            f'<text x="475" y="315" text-anchor="middle">{html.escape(xLabel)}</text>'
            f'<text x="75" y="24">{html.escape(yLabel)}</text></svg>')


def writePlot(result, frequencies, power, values, path):
    """Write the measurements and four graphs into one self-contained HTML file.

    analyzeNoise supplies result, the completed report dictionary; frequencies and
    power, the FFT arrays; values, individual readings in microvolts; and path, the
    output filename. This report assumes the test's 1,024,000 readings per second.
    It writes the file and returns no value; file errors propagate to the caller.

    Show every reading in the first 2 ms, then a histogram counting all readings in
    100 voltage intervals. Show every FFT bin from 0 to 500 Hz. For the full-frequency
    graph, combine bins into display groups but keep the strongest point in each
    group, so narrow peaks remain visible. This reduces drawing size only; statistics
    and peak detection still use the complete arrays.
    """
    count = min(len(values), int(sampleRate * .002))
    plots = svgPlot('Individual readings: first 2 ms of the analyzed second',
                    np.arange(count) / sampleRate * 1000, values[:count], 'Milliseconds', 'Microvolts relative to ground')
    counts, edges = np.histogram(values, bins=100)
    plots += svgPlot('Distribution of all 1,024,000 readings', (edges[:-1] + edges[1:]) / 2, counts,
                     'Microvolts relative to ground', 'Number of readings')
    low = frequencies <= 500
    plots += svgPlot('FFT: 0-500 Hz, including 60 Hz and its multiples', frequencies[low], np.sqrt(power[low]),
                     'Frequency (Hz)', 'Noise density (uV/sqrt(Hz)); logarithmic vertical scale', logarithmicY=True)

    # Space display-group boundaries by a constant ratio to suit the logarithmic
    # frequency axis. Keep the maximum in each group rather than an average.
    # Integer rounding can repeat a boundary, so remove duplicates first.
    boundaries = np.unique(np.geomspace(1, len(power), 1800).astype(int))
    selected = [int(begin + np.argmax(power[begin:end])) for begin, end in zip(boundaries[:-1], boundaries[1:]) if end > begin]
    plots += svgPlot('FFT: full measured frequency range', frequencies[selected], np.sqrt(power[selected]),
                     'Frequency (Hz), logarithmic scale', 'Noise density (uV/sqrt(Hz)); logarithmic vertical scale', True, True)

    # Embed the plots and report text in the file so viewing needs no internet.
    path.write_text('<!doctype html><html lang="en"><meta charset="utf-8"><title>Grounded-input noise</title>'
                    '<style>body{font:17px Arial;max-width:1100px;margin:30px auto;padding:20px;color:#203040}'
                    'pre{white-space:pre-wrap;line-height:1.5}svg{width:100%;background:#fafcff;font:13px Arial}h2{margin-top:40px}</style>'
                    '<h1>Grounded-input noise</h1><pre>' + html.escape(summarize(result)) + '</pre>'
                    '<p>Recorded normally to SD. Analyzed one second starting one second into retained data. '
                    'Statistics use every raw reading; mean offset is removed only for the FFT. '
                    'Hann window; no FFT averaging; 1 Hz bin spacing, 1.5 Hz window noise bandwidth. '
                    'An FFT is not proof of randomness. '
                    'The FFT covers 0-512 kHz; ADC filtering can shape the noise, so random noise need not have a flat spectrum. '
                    'The short waveform is a view of 2 ms, not a summary of all fluctuations. '
                    'Narrow peaks are candidates for investigation, not identification of their source.</p>'
                    '<p><a href="noise-samples.bin">Original signed 24-bit readings</a>: lowest byte first, '
                    '1,024,000 readings. Convert each signed code to volts using code × 2.5 / 8,388,608.</p>'
                    + plots + '</html>', encoding='utf-8')
