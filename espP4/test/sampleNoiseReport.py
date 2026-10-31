# Copyright (C) 2026 Karl Stock
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure fluctuations that are hidden by the slow square-wave average plot.

The P4 groups readings into 100 ms sections called bins. For each bin it reports
the sample count, mean, smallest and largest readings, and the sum of squared
differences from the mean. These allow the PC to calculate individual-sample
variance without transferring every recorded sample over USB.

addSampleStatistics combines the bins chosen by the square-wave analysis,
excluding sections near voltage transitions. plotSamples shows short sequences
of individual readings. The combined statistics include drift between bins and
fluctuations from the generator and input circuit, not just the ADC.
"""
import math
import statistics

microvoltsPerCount = 2500000.0 / 8388608
traceSamples = 128
traceBins = 40


def addSampleStatistics(text, result, rate):
    """Read the reported statistics and add low/high sample statistics to result.

    result['settledBins'] identifies flat sections selected by the waveform
    analysis. Their sample counts and means have already been checked by the
    recording parser. M2 means the sum of squared differences from a bin's mean,
    measured in ADC counts squared; dividing by count gives its variance.
    Reject missing, duplicate or inconsistent rows rather than reporting an
    artificially small noise value. rate is samples per second.
    """
    headers = [line for line in text.splitlines() if line.startswith('SMOKE_NOISE ')]
    if headers != [f'SMOKE_NOISE version=1 traceSamples={traceSamples} traceBins={traceBins}']:
        raise ValueError('Sample statistics missing or unsupported; upload the new P4 smoke firmware')
    bins, moments, traces = {}, {}, {}
    for line in text.splitlines():
        fields = line.split(',')
        if fields[0] == 'SMOKE_SD':
            bins[int(fields[1])] = (int(fields[2]), float(fields[3]))
        elif fields[0] == 'SMOKE_MOMENT':
            if len(fields) != 5:
                raise ValueError('Malformed sample-statistics row')
            iBin, m2, minimum, maximum = int(fields[1]), float(fields[2]), int(fields[3]), int(fields[4])
            if iBin in moments or not math.isfinite(m2) or m2 < 0 or not -8388608 <= minimum <= maximum <= 8388607:
                raise ValueError('Invalid or repeated sample-statistics row')
            moments[iBin] = (m2, minimum, maximum)
        elif fields[0] == 'SMOKE_TRACE':
            if len(fields) != traceSamples + 2:
                raise ValueError('Incomplete raw sample trace')
            iBin = int(fields[1])
            values = list(map(int, fields[2:]))
            if iBin in traces or any(value < -8388608 or value > 8388607 for value in values):
                raise ValueError('Invalid or repeated raw sample trace')
            traces[iBin] = values
    if set(moments) != set(bins):
        raise ValueError('Sample statistics do not cover the recorded bins')
    for iBin, (count, mean) in bins.items():
        m2, minimum, maximum = moments[iBin]
        if count <= 0 or not minimum - 1e-5 <= mean <= maximum + 1e-5:
            raise ValueError('Sample mean is outside reported extrema')
        # Firmware prints nine significant digits. Allow that rounding when
        # checking the maximum possible variance for the reported code range.
        if m2 > count * (maximum-minimum)**2 / 4 * (1 + 1e-8) + 1:
            raise ValueError('Sample variance exceeds the reported range')
    for iBin, values in traces.items():
        if iBin not in bins or iBin - min(bins) >= traceBins:
            raise ValueError('Raw sample trace is outside its retained window')
        _, minimum, maximum = moments[iBin]
        if min(values) < minimum or max(values) > maximum:
            raise ValueError('Raw sample trace contradicts bin extrema')

    combined = {}
    for name in ('low', 'high'):
        selected = result['settledBins'][name]
        count, mean, m2 = 0, 0.0, 0.0
        minimum, maximum = 8388607, -8388608
        for iBin in selected:
            binCount, binMean = bins[iBin]
            binM2, binMinimum, binMaximum = moments[iBin]
            # Combine this bin with the accumulated samples. The delta term
            # includes differences between bin means, so slow drift is not
            # discarded when calculating the overall variance.
            total = count + binCount
            delta = binMean - mean
            m2 += binM2 + delta * delta * count * binCount / total
            mean += delta * binCount / total
            count = total
            minimum, maximum = min(minimum, binMinimum), max(maximum, binMaximum)
        available = [iBin for iBin in selected if iBin in traces]
        if selected and not available:
            raise ValueError(f'No settled raw sample trace for {name} level')
        if count:
            iBin = available[0]
            values = traces[iBin]
            traceMean = statistics.mean(values)
            variance = m2 / count * microvoltsPerCount**2
            combined[name] = dict(samples=count, meanV=mean*microvoltsPerCount/1000000,
                                  varianceUv2=variance, stddevUv=math.sqrt(variance),
                                  minimumV=minimum*microvoltsPerCount/1000000,
                                  maximumV=maximum*microvoltsPerCount/1000000,
                                  peakToPeakUv=(maximum-minimum)*microvoltsPerCount,
                                  traceStartSeconds=iBin/10 + .05, traceRate=rate,
                                  traceCodes=values, traceMeanV=traceMean*microvoltsPerCount/1000000,
                                  traceResidualUv=[(value-traceMean)*microvoltsPerCount for value in values])
        else:
            combined[name] = None
    result['sampleStatistics'] = combined


def summarizeSamples(result):
    """Return readable sample counts and fluctuation measurements for each voltage level."""
    lines = ['Individual samples in settled sections (includes source fluctuation and drift):']
    for name, values in result['sampleStatistics'].items():
        if values is not None:
            lines.append(f"{name.capitalize()}: {values['samples']:,} samples; standard deviation {values['stddevUv']:.3f} uV; "
                         f"variance {values['varianceUv2']:.3f} uV^2; min {values['minimumV']:.6f} V; "
                         f"max {values['maximumV']:.6f} V; peak-to-peak {values['peakToPeakUv']:.3f} uV")
    return lines


def plotSamples(result):
    """Draw 128 consecutive readings from one flat section at each voltage level.

    Subtract each displayed section's mean so the small fluctuations are visible.
    Both panels use the same vertical scale. This display adjustment does not
    change the statistics, which use all samples in the selected flat sections.
    """
    records = [values for values in result['sampleStatistics'].values() if values is not None]
    scale = max([abs(value) for values in records for value in values['traceResidualUv']] + [1.0]) * 1.1
    panels = ['<h2>Individual samples: fluctuations hidden by averaging</h2><p>128 consecutive samples per panel. Each trace has its own mean subtracted. Both panels use the same vertical scale. These short examples do not show the full recorded extremes.</p>']
    for name, values in result['sampleStatistics'].items():
        if values is not None:
            rate = values['traceRate']
            residuals = values['traceResidualUv']
            parts = []
            for iTick in range(5):
                level = -scale + 2*scale*iTick/4
                y = 40 + (scale-level)*250/(2*scale)
                parts.append(f'<line x1="100" y1="{y}" x2="1080" y2="{y}" stroke="#ddd"/><text x="10" y="{y}">{level:.2f} uV</text>')
                timeUs = (len(residuals)-1)*1000000/rate*iTick/4
                x = 100+980*iTick/4
                parts.append(f'<text x="{x}" y="320" text-anchor="middle">{timeUs:.1f}</text>')
            coords = [(100+980*iSample/(len(residuals)-1), 40+(scale-value)*250/(2*scale)) for iSample, value in enumerate(residuals)]
            line = ' '.join(f'{x:.2f},{y:.2f}' for x,y in coords)
            parts.append(f'<polyline points="{line}" fill="none" stroke="#166ac1" stroke-width="1.5"/>')
            for iSample, (x,y) in enumerate(coords):
                parts.append(f'<circle cx="{x}" cy="{y}" r="2" fill="#166ac1"><title>{iSample*1000000/rate:.3f} us: {residuals[iSample]:+.3f} uV</title></circle>')
            panels.append(f'<h3>{name.capitalize()} level: starting {values["traceStartSeconds"]:.3f} s into recording</h3><p>Trace mean: {values["traceMeanV"]:.6f} V</p><svg viewBox="0 0 1140 365" role="img" aria-label="{name} individual sample fluctuations">{"".join(parts)}<text x="590" y="355" text-anchor="middle">Microseconds since first displayed sample</text></svg>')
    return ''.join(panels)
