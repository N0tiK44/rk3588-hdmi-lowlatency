# V3.8 Orange Pi runbook

## Run and package

From the normal Orange Pi login account, use the existing checkout:

```bash
bash ~/src/rk3588-hdmi-lowlatency/scripts/pi-debug.sh v38
```

The wrapper refuses local changes, fast-forwards `main`, runs `make clean`,
`make check`, and `make`, then executes two 120-second 59.940 Hz arms. It writes
the stable archive path:

```text
~/hdmirx-results/latest.tar.gz
```

From PowerShell in the repository on the Windows PC:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\fetch-latest.ps1
```

## What to verify

The report and early-arm log must show:

- four capture buffers;
- zero missing Rockchip fences;
- `runtime status: completed`;
- the V3.8 early-DQ, overlap, and intentional-drop counters;
- raw, intentional, and unexpected sequence gaps separately.

`V3.8 overlap EBUSY: 1` is an expected kernel capability result. If capture
never becomes ready before the current OUT fence, zero overlap attempts plus
`no capture-ready-before-OUT window observed` is also a valid and important
result: the current phase offers nothing that userspace can submit early. One raw gap
paired with one intentional drop and zero unexpected gaps is valid. A
missing fence, unexpected gap, failed runtime, or repeated uncontrolled drop is
not valid.

The test is two arms, so the default runtime is roughly four minutes plus build
and packaging time. It stops by itself; do not press Ctrl-C unless it has
clearly hung beyond both arms.
