# RT budget negative fixture

Consumed only by `scripts/rt-budgets-check.py --expect-fail --expect-error`.

```rt-budget
class: LKL network request round trip
command: c
config-hash: sha256:abc
target: t
platform: qemu-only PB-IOMMU-RISCV
opensbi: o
kernel-git-sha: s
date: 2026-07-07
samples: 1
max-ns: 1
p99-ns: 1
histogram-artifact: x
```
