# Benchmarking a board

The one question that decides whether sc55d is usable on a given machine is
whether it can render audio at least as fast as the audio plays. `--bench`
answers it with a number, and sets its exit status accordingly, so it works in a
script.

It is the go/no-go number for a board: it loads the ROMs, feeds a dense
sixteen-part sequence generated in code (no external file), renders 30 seconds
of audio as fast as the machine allows, discards the samples, and reports how
many seconds of audio it produced per wall-clock second.

```bash
./build/sc55d --roms /usr/share/sc55d/roms --bench
```

```
  rendered      30.00 s of audio (1986210 frames at 66207 Hz)
  wall clock    ...
  realtime      ...x  (rendered seconds per wall-clock second)
  worst second  ...x

  verdict       ...
```

A one-second warm-up (firmware boot plus a GS reset) runs before the clock
starts. **`worst second` is the number that matters** — the lowest ratio over
any one second of the run. The average can hide a stall that would be an xrun in
real use. At or above 1.0x holds realtime; the exit status is 0 when it does and
1 when it does not.

A one-second warm-up is not enough with real ROMs — the firmware has not booted
and the run measures silence, which the digest line flags as `(SILENT)`. The
default is 4 s (`--bench-warmup`).

Run it the way the daemon will run: same `--cpu`, same `--model`, same
privileges. And compare like with like — the ratio moves several-fold with build
flags, so re-benchmark after changing them.

**Run it with `--no-realtime`.** Under `SCHED_FIFO` the benchmark saturates its
core, which is exactly what the kernel RT throttle punishes: the tail of the
distribution then measures the throttle rather than the emulator. See
[Performance § System tuning](performance.md#system-tuning).

## Where the time goes within a period

`--bench-histogram` times every individual period and reports the distribution
against that period's realtime budget:

```
./build/sc55d --roms /path/to/roms --bench --bench-histogram --no-realtime
```

It prints mean, median, p90, p99, p99.9 and max as multiples of the budget, how
many periods went over it, the worst run of consecutive over-budget periods, and
the **peak cumulative deficit** — the running sum of how far behind realtime the
renderer fell, floored at zero. That last one is the number that says how deep
`--render-ahead` has to be, and it is the only one that does: the
consecutive-run figure reads 1 almost always, because a single 14 ms stall lands
inside one period rather than spreading across several, and sizing a ring from
it would give you 2 and be badly wrong.

`--bench-ring` pushes every period through a real `PeriodRing` to a consumer
thread that discards it, so the cost of the hand-off can be priced against the
same run without it. Measured here: **10–20 µs per period**, about 0.5% of the
budget at 256 frames — a 1 KiB memcpy, a mutex pair and one futex wake. It is a
fixed per-period cost, so it hurts proportionally more at smaller periods
(0.74% at 128), and on a board running near its limit it eats a larger share of
what slack is left.

