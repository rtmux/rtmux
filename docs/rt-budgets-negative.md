# RT budget negative fixture

This file is consumed only by `scripts/rt-budgets-check.py --expect-fail`.

```perf-evidence
claim: invalid performance evidence fixture
metric: p99 latency
command: false
config: fixture
baseline: 10 ns in fixture-before.log
rollback-threshold: any regression
```
