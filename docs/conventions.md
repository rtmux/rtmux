# Coding Conventions

- Language and ABI: C, LP64 only; no ILP32 or big-endian paths.
- Formatting: K&R, 4-space indentation (no tabs), 100-column soft limit.
- Types: snake_case with a `_t` suffix (`thread_t`, `process_t`, `channel_t`,
  `connect_t`).
- Assembly: `.S` files (preprocessed); arch-portable interfaces use a `cpu_`
  prefix.
- Freestanding flags: `-nostdinc -nostdlib -ffreestanding -Wconversion
  -fno-strict-aliasing -march=rv64g -mcmodel=medany -mno-relax`, with
  `-ffunction-sections -fdata-sections` and `--gc-sections`. The tree builds
  warning-clean; keep it that way.
- Unfinished work is marked `FIXME` or `TODO`.
- Licensing: Apache 2.0 throughout.
