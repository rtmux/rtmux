# Fixture contracts

| id | assumption | class | evidence | hardware gate |
| --- | --- | --- | --- | --- |
| PB-FX-HW | Hardware-backed assumption. | production | `README.md` records a board run on the Unmatched | `README.md` board procedure |
| PB-FX-GATED | Emulator assumption with a gate. | qemu-only | qemu run | `scripts/fu740-irqoff-budget.sh` |
| PB-FX-UNGATED | Emulator assumption, no gate. | qemu-only | qemu run | none |
| PB-FX-FIXTURE | Scaffold, no gate. | fixture | patched dtb | none |
