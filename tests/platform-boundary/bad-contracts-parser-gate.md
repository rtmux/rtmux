# Fixture contracts: a synthetic-log parser check offered as a hardware gate

<!-- expect: hardware gate is a synthetic-log parser check -->

| id | assumption | class | evidence | hardware gate |
| --- | --- | --- | --- | --- |
| PB-FX-X | Assumption. | qemu-only | qemu run | `make check-fu740-smoke-scripts` |

platform: qemu-only PB-FX-X
