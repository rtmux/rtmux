# Meta: expected error plus an unrelated one

<!-- expect: unknown platform contract PB-FX-MISSING -->

```rt-budget
class: x
platform: qemu-only PB-FX-MISSING
```

```rt-budget
class: y
platform: bogus
```
