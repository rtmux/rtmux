#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare assertion identities in two successful native conformance transcripts."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import sys


def assertions(path):
    data = path.read_bytes()
    summary = re.search(rb'=== Results: (\d+) tests, (\d+) passed, (\d+) failed ===', data)
    if not summary:
        raise ValueError(f'{path}: missing summary')
    total, passed, failed = map(int, summary.groups())
    if not total or failed or total != passed:
        raise ValueError(f'{path}: first suite did not pass')
    names = Counter(s.decode() for s in re.findall(rb'(?m)  PASS  (.*?)\r?$', data[:summary.start()]))
    if sum(names.values()) != passed:
        raise ValueError(f'{path}: assertion labels disagree with summary')
    return names, hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline', type=Path)
    parser.add_argument('candidate', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    report = {'status': 'FAIL', 'baseline': str(args.baseline), 'candidate': str(args.candidate)}
    try:
        base, base_hash = assertions(args.baseline)
        candidate, candidate_hash = assertions(args.candidate)
        report.update(baseline_passed=sum(base.values()), candidate_passed=sum(candidate.values()),
                      baseline_log_sha256=base_hash, candidate_log_sha256=candidate_hash,
                      only_baseline=dict(base - candidate), only_candidate=dict(candidate - base))
        if base != candidate:
            raise ValueError('Assertion inventories differ; compare equivalent configurations')
        report['status'] = 'PASS'
        print(f'Native parity: PASS ({sum(base.values())} identical passing assertions)')
        return 0
    except (ValueError, OSError) as exc:
        report['error'] = str(exc)
        print(f'Native parity: FAIL: {exc}', file=sys.stderr)
        return 1
    finally:
        args.out.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    sys.exit(main())
