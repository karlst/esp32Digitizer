"""Reconstruct square-wave transitions from saved, asynchronously sampled edges.

Read main, then fitCrossing and writePlot. Each saved ADC reading retains its
time relative to the direct GPIO6 edge. Combining different edges fills gaps
between the readings of any single edge; no transition is shifted to improve
agreement. Falling edges were already normalized to rise from zero to one.

A local cubic curve is fitted near halfway, and its 50% crossing is calculated.
Other window widths and polynomial degrees check dependence on that choice.
These checks are not an absolute uncertainty bound. No hardware or original
data is changed. Run this script on the PC with NumPy installed.
"""
import json
from pathlib import Path
import numpy as np


def fitCrossing(times, values, width=500, degree=3):
    """Fit normalized voltage near 2550 ns and return its unique halfway crossing.

    times are ns after the direct input edge; values are normalized ADC readings.
    width is the half-width in ns of the fitting interval. All selected readings
    receive equal weight. Scaling time to microseconds improves numerical
    conditioning. Polynomial fitting uses least squares, not a physical model
    of the ADC filter. Return a JSON-ready record and the polynomial coefficients.
    """
    selected = abs(times - 2550) < width
    x = (times[selected] - 2550) / 1000
    y = values[selected] - 0.5
    if len(x) < degree + 2:
        raise ValueError('Not enough readings to fit the crossing')
    coefficients = np.polynomial.polynomial.polyfit(x, y, degree)
    roots = np.polynomial.polynomial.polyroots(coefficients)
    crossings = [2550 + root.real * 1000 for root in roots
                 if abs(root.imag) < 1e-8 and abs(root.real * 1000) < width]
    if len(crossings) != 1:
        raise ValueError('The fitted window does not have a unique 50% crossing')
    residual = y - np.polynomial.polynomial.polyval(x, coefficients)
    return {'crossingNs': float(crossings[0]), 'points': len(x), 'halfWidthNs': width,
            'degree': degree, 'residualRmsFraction': float(np.sqrt(np.mean(residual ** 2)))}, coefficients


def writePlot(path, groups):
    """Draw four local fits, showing raw points and the fitted halfway crossings.

    Each panel uses identical axes: 2050..3050 ns and 20..80% of the transition.
    The selected cubic is shown only within its fitted window. Points are actual
    saved readings from different transitions, not interpolated extra samples.
    """
    parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="850" viewBox="0 0 1200 850">',
             '<rect width="1200" height="850" fill="white"/>',
             '<g font-family="Arial,sans-serif" fill="#233347">',
             '<text x="60" y="35" font-size="25">Reconstructed square-wave transitions</text>',
             '<text x="60" y="64" font-size="17">Blue dots: saved ADC readings. Orange: cubic fit. No individual edge was shifted.</text>']
    for iGroup, group in enumerate(groups):
        left = 80 + (iGroup % 2) * 590
        top = 115 + (iGroup // 2) * 350
        x = lambda time: left + (time - 2050) / 1000 * 480
        y = lambda value: top + (0.8 - value) / 0.6 * 240
        parts.append(f'<text x="{left}" y="{top-15}" font-size="19">{group["frequencyHz"]} Hz — {group["polarity"]}</text>')
        for fraction in (0.2, 0.5, 0.8):
            parts.append(f'<path d="M{left} {y(fraction)}h480" stroke="#ccd4dd"/><text x="{left-12}" y="{y(fraction)+5}" font-size="14" text-anchor="end">{fraction*100:.0f}%</text>')
        for time in (2100, 2300, 2500, 2700, 2900):
            parts.append(f'<text x="{x(time)}" y="{top+263}" font-size="14" text-anchor="middle">{time}</text>')
        for time, value in zip(group['times'], group['values']):
            if 2050 <= time <= 3050 and 0.2 <= value <= 0.8:
                parts.append(f'<circle cx="{x(time):.2f}" cy="{y(value):.2f}" r="2.4" fill="#2677ae" opacity="0.5"/>')
        plotTimes = np.linspace(2050, 3050, 201)
        plotValues = 0.5 + np.polynomial.polynomial.polyval((plotTimes-2550)/1000, group['coefficients'])
        points = ' '.join(f'{x(t):.2f},{y(v):.2f}' for t, v in zip(plotTimes, plotValues))
        crossing = group['fit']['crossingNs']
        parts.extend([f'<polyline points="{points}" fill="none" stroke="#bf531b" stroke-width="2.5"/>',
                      f'<path d="M{x(crossing)} {y(.5)}V{top+240}" stroke="#bf531b" stroke-dasharray="5 4"/>',
                      f'<text x="{left+255}" y="{top+225}" font-size="16">50% at {crossing:.2f} ns</text>',
                      f'<text x="{left+240}" y="{top+292}" font-size="15" text-anchor="middle">Time after GPIO6 input edge (ns)</text>'])
    parts.append('<text x="60" y="825" font-size="16">Falling transitions were normalized to 0–100%. P4 input timing correction was not subtracted.</text></g></svg>')
    path.write_text('\n'.join(parts), encoding='utf-8')


def main():
    """Analyze two completed five-capture sessions, retaining polarities separately.

    Coverage is measured in the 2000..3000 ns region around the crossing.
    Per-trial fits check variation between captures. Sensitivity estimates
    change curve degree and window size without shifting or discarding edges.
    The existing client document is not modified.
    """
    root = Path(__file__).resolve().parents[2] / 'captures' / 'pulse-delay'
    summaries = []
    plots = []
    for folder, frequency in (('20261105-153442', 997), ('20261105-170932', 10000)):
        trials = json.loads((root / folder / 'result.json').read_text(encoding='utf-8'))
        for rising in (True, False):
            edges = [edge for trial in trials for edge in trial['edges'] if edge['rising'] == rising]
            times = np.array([t for edge in edges for t in edge['timesNs']])
            values = np.array([v for edge in edges for v in edge['normalized']])
            fit, coefficients = fitCrossing(times, values)
            coverage = np.sort(times[(times > 2000) & (times < 3000)])
            sensitivities = [fitCrossing(times, values, width, degree)[0]
                             for degree in (2, 3, 4) for width in (350, 500, 700, 900)]
            trialFits = []
            for trial in trials:
                selected = [edge for edge in trial['edges'] if edge['rising'] == rising]
                trialTimes = np.array([t for edge in selected for t in edge['timesNs']])
                trialValues = np.array([v for edge in selected for v in edge['normalized']])
                trialFits.append(fitCrossing(trialTimes, trialValues)[0])
            linearMean = float(np.mean([edge['halfHeightNs'] for edge in edges]))
            summary = {'session': folder, 'frequencyHz': frequency,
                       'polarity': 'rising' if rising else 'falling', 'edges': len(edges),
                       'fit': fit, 'linearCrossingMeanNs': linearMean,
                       'curveMinusLinearNs': fit['crossingNs'] - linearMean,
                       'coverageMaxGapNs': float(np.max(np.diff(coverage))),
                       'pointsPer100NsBin': np.histogram(coverage, bins=np.arange(2000, 3001, 100))[0].tolist(),
                       'trialFits': trialFits, 'sensitivityFits': sensitivities}
            summaries.append(summary)
            plots.append({**summary, 'times': times, 'values': values, 'coefficients': coefficients})
            print(json.dumps({key: summary[key] for key in ('frequencyHz', 'polarity', 'fit',
                              'linearCrossingMeanNs', 'curveMinusLinearNs', 'coverageMaxGapNs')}))
            print('Trial crossings:', [round(row['crossingNs'], 3) for row in trialFits])
            print('All model/window crossings:', min(row['crossingNs'] for row in sensitivities),
                  max(row['crossingNs'] for row in sensitivities))
    report = {'method': 'Unshifted normalized transitions; local cubic least squares, 2050..3050 ns.',
              'limitation': 'No absolute accuracy bound; sensitivity and trial spread are descriptive. '
                            'No P4 input-skew correction. All original transitions retained.',
              'groups': summaries}
    (root / 'transitionCurveComparison.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    writePlot(root / 'transitionCurveComparison.svg', plots)


if __name__ == '__main__':
    main()
