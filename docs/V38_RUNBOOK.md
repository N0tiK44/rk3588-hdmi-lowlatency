# V3.8 retired

The V3.8 hardware run proved that its intentional phase-prime drop added one
frame. The selector now refuses to run so the regression cannot be repeated by
accident.

Use the corrected [V3.8.1 runbook](V381_RUNBOOK.md):

```bash
bash ~/src/rk3588-hdmi-lowlatency/scripts/pi-debug.sh v381
```

The original result remains documented in
[`docs/results/v3.8-early-submit.md`](results/v3.8-early-submit.md).
