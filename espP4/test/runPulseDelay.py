# Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
"""Capture ADC path delay using the dedicated P4 pulse-delay image.

GPIO6 receives the direct 3.3-V logic pulse; CH0 receives the reduced analog
branch. Use 997 Hz, 1000 Hz or 10000 Hz, 50% duty. Confirm wiring
and voltages per docs/PULSE-DELAY-TEST.md before enabling the generator.
This script does not build, flash, change generator settings or access SD.

This is only the command-line entry point. Read runFsyncAssociation.capture for
the two-board handshake and logging, then pulseDelayReport.analyze for the delay
calculation. --frequency checks the manually selected stimulus. It does not
change the ADC sample rate, which remains 1,024,000 readings per second.
"""
import argparse
from pathlib import Path
from runFsyncAssociation import capture


def main():
    """Parse port/rate options and delegate hardware runs to the shared runner.

    The runner writes raw logs, JSON and HTML to a new session directory. This
    entry point reports exceptions or Ctrl+C as exit status 1; reaching the end
    means requested captures were analyzed, not that accuracy met a chosen bound.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--feather', default='COM4')
    parser.add_argument('--p4', default='COM3')
    parser.add_argument('--runs', type=int, default=5)
    parser.add_argument('--frequency', type=int, choices=(997, 1000, 10000), default=997,
                        help='Expected generator frequency in Hz; does not set the generator')
    parser.add_argument('--link-only', action='store_true', help='Check serial communication only; leave generator output off')
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parents[2] / 'captures/pulse-delay')
    args = parser.parse_args()
    try:
        capture(args.feather, args.p4, args.runs, args.output, delay=True,
                linkOnly=args.link_only, frequencyHz=args.frequency)
    except (Exception, KeyboardInterrupt) as error:
        print('FAIL:', str(error) or 'Interrupted')
        raise SystemExit(1)


if __name__ == '__main__':
    main()
