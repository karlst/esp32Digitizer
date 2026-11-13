"""Compare two arrival-time definitions using saved square-wave ADC readings.

Run this PC-only analysis after the original pulse captures. No hardware is
accessed, and no calibration or source capture is changed. Read main, then
measureTransition and summarize. Results are written beside the capture folders.

The two completed five-trial sessions used 997 Hz and 10 kHz. Each edge supplies
40 normalized ADC readings and their FSYNC times relative to the GPIO6 edge.
Both estimates use those same points. Their difference isolates the calculation
change; it does not remove input threshold, wiring, or capture timing errors.
"""
import csv
import json
from pathlib import Path
import statistics


def summarize(values):
    """Describe observed values in ns; the population SD is not a confidence bound."""
    return {
        'count': len(values), 'meanNs': statistics.mean(values),
        'medianNs': statistics.median(values), 'sdNs': statistics.pstdev(values),
        'minNs': min(values), 'maxNs': max(values),
    }


def measureTransition(edge):
    """Recalculate weighted center and 50% crossing from one saved transition.

    Falling transitions were normalized from zero to one by the original
    analysis, so the same calculation applies to either polarity. The crossing
    is placed on the straight line connecting the two samples around 50%.
    The weighted center retains signed voltage changes, including ringing.
    Return both times in ns after the direct GPIO6 edge. Reject ambiguous
    crossings or disagreement with the original stored calculations.
    """
    times = edge['timesNs']
    values = edge['normalized']
    crossings = []
    for iSample in range(1, len(values)):
        if values[iSample - 1] < 0.5 <= values[iSample]:
            fraction = (0.5 - values[iSample - 1]) / (values[iSample] - values[iSample - 1])
            crossings.append(times[iSample - 1] + fraction * (times[iSample] - times[iSample - 1]))
    if len(crossings) != 1:
        raise ValueError('Transition does not have exactly one 50% crossing')

    # Use the original center's sample window so only the estimator changes.
    changes = [values[i] - values[i - 1] for i in range(8, 33)]
    center = sum(change * (times[i - 1] + times[i]) / 2
                 for i, change in zip(range(8, 33), changes)) / sum(changes)
    halfHeight = crossings[0]
    if abs(center - edge['centerNs']) > 1e-6 or abs(halfHeight - edge['halfHeightNs']) > 1e-6:
        raise ValueError('Recalculation disagrees with the saved report')
    return center, halfHeight


def main():
    """Read completed sessions and save paired edge/trial summaries and a CSV.

    Every accepted edge is retained. Summaries pool edges within each polarity;
    separate trial summaries allow the reader to inspect capture variation.
    No independence assumption or confidence interval is imposed on the edges.
    No GPIO input-skew correction is subtracted from either method, so it cancels
    in their paired difference. The incomplete earlier session is excluded.
    """
    root = Path(__file__).resolve().parents[2] / 'captures' / 'pulse-delay'
    rows = []
    sessions = []
    for folder, frequency in (('20261105-153442', 997), ('20261105-170932', 10000)):
        trials = json.loads((root / folder / 'result.json').read_text(encoding='utf-8'))
        sessionRows = []
        for iTrial, trial in enumerate(trials, 1):
            for edge in trial['edges']:
                center, crossing = measureTransition(edge)
                sessionRows.append({
                    'session': folder, 'frequencyHz': frequency, 'trial': iTrial,
                    'edgeIndex': edge['index'], 'polarity': 'rising' if edge['rising'] else 'falling',
                    'centerNs': center, 'halfHeightNs': crossing,
                    'differenceNs': crossing - center,
                })

        # Positive differences mean interpolation reports a later arrival.
        summaries = []
        for trialNumber in [None] + list(range(1, len(trials) + 1)):
            for polarity in ('rising', 'falling'):
                selected = [row for row in sessionRows if row['polarity'] == polarity
                            and (trialNumber is None or row['trial'] == trialNumber)]
                summaries.append({'trial': trialNumber, 'polarity': polarity,
                                  **{key: summarize([row[key] for row in selected])
                                     for key in ('centerNs', 'halfHeightNs', 'differenceNs')}})
        sessions.append({'session': folder, 'frequencyHz': frequency, 'trials': len(trials),
                         'summaries': summaries})
        rows.extend(sessionRows)

    report = {'description': '50% linear interpolation minus weighted transition center; all times in ns.',
              'limitation': 'Observed paired differences, not absolute accuracy or a confidence interval. '
                            'No input-skew correction applied. Original captures unchanged.',
              'recalculationMatchesSavedResults': True, 'sessions': sessions}
    (root / 'methodComparison.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    with (root / 'methodComparison.csv').open('w', newline='', encoding='utf-8') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    for session in sessions:
        for summary in session['summaries'][:2]:
            print(session['frequencyHz'], summary['polarity'], json.dumps(summary))


if __name__ == '__main__':
    main()
