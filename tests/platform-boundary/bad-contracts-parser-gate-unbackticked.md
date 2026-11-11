# Fixture contracts: synthetic gate named without backticks

<!-- expect: synthetic-log parser check -->

| id | assumption | class | evidence | hardware gate |
| --- | --- | --- | --- | --- |
| PB-FX-X | Assumption. | qemu-only | qemu run | make -s check-fu740-smoke-scripts |

platform: qemu-only PB-FX-X
