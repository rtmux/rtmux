# Fixture contracts: production row whose gate file does not exist

<!-- expect: hardware gate names a missing file -->

| id | assumption | class | evidence | hardware gate |
| --- | --- | --- | --- | --- |
| PB-FX-X | Assumption. | production | `README.md` board run on the Unmatched | `scripts/nope-hw.sh` |

platform: production PB-FX-X
