# Linux per-CPU usage sampling aliasing demos

Linux tracks CPU time for two different consumers: the per-CPU counters in
`/proc/stat`, which `top` and `mpstat` display, and the per-task counters in
`/proc/self/stat`. The two sets of counters are produced by different accounting
paths, so they do not have to agree.

This repository contains two small demo programs that push that disagreement to
opposite extremes:

- `too-much-per-cpu-usage` makes Linux report a large per-CPU usage (about 50 %)
  for a single thread that consumes almost no processor time.
- `too-little-per-cpu-usage` makes Linux report almost no per-CPU usage (0.00 %)
  for a single thread that consumes almost all of a processor.

Both exploit the same underlying fact. The per-CPU busy counters are updated by
*sampling* at the kernel tick, while the per-CPU idle counter is measured
*precisely*. By arranging a short periodic interruption (a timerfd-driven busy
loop or sleep) to land exactly on the sampling instants, a task can control
whether the kernel attributes each tick to the running task or to the idle task.
Because the sampling phase is not known in advance and differs between
executions, each program uses a feedback loop to discover it at run time and
then holds it.

## The exploited accounting

Linux samples per-CPU usage at the kernel tick rate (`CONFIG_HZ`, 250 Hz on the
test machine, i.e. one sample every 4 ms). At each sample,
`account_process_tick()` adds exactly one `TICK_NSEC` of time to whatever the
CPU is currently doing. Those charges are what fill the `user`, `nice`,
`system`, `irq`, `softirq` and `steal` fields of `/proc/stat`.

The `idle` field is produced differently. `/proc/stat` does not use the
tick-accumulated `kcpustat.idle`; `get_idle_time()` uses
`get_cpu_idle_time_us()`, the NOHZ accumulator (`ts->idle_sleeptime`), which the
kernel documents as being "measured via accounting rather than sampling, and is
as accurate as `ktime_get()`". The idle field therefore records the real idle
time of the core, including intervals in which a tick was charged as busy.

`/proc/stat` displays all of these counters in `USER_HZ` (100 Hz). Tools compute
per-CPU usage as:

```
usage = busy / total,   where total = busy + idle
```

Because the same wall interval can be counted both as a sampled busy tick and as
precisely-measured idle, the two terms can overlap, and the reported usage can
differ greatly from the real CPU time consumed by the task.

A periodic timerfd whose period shares a common base frequency with the tick can
lock a short interruption to the sampling instants. The phase of the sampling
instants is not exposed to user space and varies between executions, so it must
be found empirically.

## The two effects

### Exaggerated per-CPU usage

If a task runs a very short busy loop at every sampling instant and sleeps in
between, then every sample charges a full tick to the task even though it ran
for only microseconds. The busy counter saturates at one tick per tick, i.e. one
full wall's worth of busy time, while the precise idle counter still records the
real idle time. The reported usage therefore approaches 50 % as the real work
approaches zero:

```
usage = 1 / (2 − d)
```

where `d` is the real duty cycle. The full derivation, the raw counters and the
measured table are given in
[Why the exaggerated usage is capped at 50 %](#why-the-exaggerated-usage-is-capped-at-50).

### Suppressed per-CPU usage

If instead the task spins almost continuously and inserts a very short sleep
that covers every sampling instant, then none of the samples observe the running
task; each one charges the idle task. The busy counter stays at zero while the
precise idle counter only records the short sleeps, so `usage = 0 / (0 + small)`
is effectively 0 %, even though the thread keeps consuming almost all of the
processor. This is the mirror of the first effect: the sampled busy counter
either always sees the task or never sees it.

## Finding the synchronization phase

The sampling phase differs between executions, so both programs discover it at
run time with the same kind of feedback loop:

1. Pin the thread to the selected core with `sched_setaffinity`.
2. Start with a wide interruption so that the region where the effect appears is
   easy to hit (a 50 µs busy loop for `too-much`, a 250 µs sleep for
   `too-little`).
3. Sweep the whole timerfd period to find the phase that produces the effect,
   then narrow the search window around it.
4. Halve the interruption duration and recalibrate the phase, repeating until
   the effect can no longer be sustained or a minimum duration is reached.
5. Hold the equilibrium and run forever. If the effect degrades, widen the
   interruption by one step, recalibrate the phase and reduce again, falling
   back to the full search only if the wider interruption does not recover the
   effect.

The per-CPU counters from `/proc/stat` and the per-task counters from
`/proc/self/stat` are both read during each evaluation and feed the loop.

## Build

```sh
make
```

This builds both `too-much-per-cpu-usage` and `too-little-per-cpu-usage` with
GCC and `-std=c23`. To build only one of them, use `make too-much-per-cpu-usage`
or `make too-little-per-cpu-usage`; run `make clean` to remove the binaries.

The programs are Linux-only because they use `timerfd` and `/proc`.

## Reading the output

Both programs print one line per evaluation with the same shape of metrics:

```
phase 995505 ns: per-CPU 50.25%, per-task 0.00%, real 0.55%, exaggeration 91.6x
```

- **per-CPU** — the usage of the pinned core computed from `/proc/stat`. This is
  the number `top`, `mpstat` and similar tools display for that core.
- **per-task** — the task's `utime + stime` from `/proc/self/stat`, as a
  percentage of wall time.
- **real** — the processor time the thread actually consumed, measured with
  `CLOCK_THREAD_CPUTIME_ID`.
- **exaggeration** (`too-much`) — `reported / real`, how much larger the report
  is than the real usage.
- **suppression** (`too-little`) — `real / reported` (with a small floor when the
  report is zero), how much larger the real usage is than the report.

You can confirm the numbers independently by watching the chosen core in `top`
(press `1` to show per-core lines) or by sampling `/proc/stat` directly.

## Program: `too-much-per-cpu-usage`

**Goal:** maximize the reported per-CPU usage while consuming as little
processor time as possible. The thread runs a short busy loop at each timerfd
event so that the loop covers the sampling instant, then the busy duration is
reduced to the smallest value that still sustains the effect.

Usage:

```
Usage: too-much-per-cpu-usage [OPTIONS] [PROCESSOR]

Options:
  -c, --cpu PROCESSOR      processor core to run on (default 0)
  -p, --period DURATION    timerfd period, for example 1ms (default 1ms)
  -b, --busy DURATION      starting busy loop duration per event
                           (default 50us; reduced automatically)
  -s, --settle SECONDS     settling time per phase (default 0.5)
  -r, --report SECONDS     reporting interval (default 1.0)
  -h, --help               display this message and exit
```

`DURATION` accepts a number with an optional `ns`, `us` or `ms` suffix. The
processor may be given either with `--cpu`/`-c` or as the positional argument.

Examples:

```sh
# Run on CPU 15 with the defaults.
./too-much-per-cpu-usage -c 15

# Converge faster during experimentation.
./too-much-per-cpu-usage -c 15 -s 0.1

# Start from a wider busy loop.
./too-much-per-cpu-usage -c 15 -b 250us
```

The program first verifies that the busy loop really consumes processor time,
then finds the phase with a wide busy loop and reduces it. A successful run
converges to something like:

```
Wide search: busy 50000 ns, phase 960937 ns, per-CPU 52.08%, real 5.39%.
Reduced busy: busy 25000 ns, phase 985937 ns, per-CPU 52.08%, real 2.99%.
...
Reduced busy: busy   500 ns, phase 995505 ns, per-CPU 51.02%, real 0.57%.
Equilibrium at busy 500 ns and phase 995505 ns: per-CPU 51.02%, per-task 0.00%, real 0.57%.

phase 995505 ns: per-CPU 50.25%, per-task 0.00%, real 0.55%, exaggeration 91.6x
```

The core is reported as about 50 % busy while the task consumes about 0.55 % of
a core: roughly a 90× exaggeration. Empirically 500 ns is the true floor for the
busy loop; 250 ns still gives about 43 %, while 100 ns collapses to about 6 %.

## Program: `too-little-per-cpu-usage`

**Goal:** consume as much task CPU as possible while Linux reports as little
per-CPU usage as possible. The thread spins and inserts a very short sleep once
per timerfd period so that the sleep covers the sampling instant, then the sleep
duration is reduced to the smallest value that keeps the report at zero.

Usage is the same shape as the other program, with `--sleep` instead of
`--busy`:

```
Usage: too-little-per-cpu-usage [OPTIONS] [PROCESSOR]

Options:
  -c, --cpu PROCESSOR      processor core to run on (default 0)
  -p, --period DURATION    timerfd period, for example 1ms (default 1ms)
  -l, --sleep DURATION     starting sleep duration per period
                           (default 250us; shortened automatically)
  -s, --settle SECONDS     settling time per phase (default 0.5)
  -r, --report SECONDS     reporting interval (default 1.0)
  -h, --help               display this message and exit
```

A successful run converges to a very small sleep duration and reports the real
processor usage next to the almost-zero per-CPU value:

```
Wide search: sleep 250000 ns, phase 779785 ns, per-CPU 0.00%, real 74.90%.
Reduced sleep: sleep  125000 ns, phase 967285 ns, per-CPU 0.00%, real 87.38%.
Reduced sleep: sleep    7812 ns, phase 998535 ns, per-CPU 0.00%, real 99.14%.
Equilibrium at sleep 5000 ns and phase 996093 ns: per-CPU 0.00%, per-task 89.21%, real 99.40%.

sleep 5000 ns: per-CPU 0.00%, per-task 99.46%, real 99.39%, suppression 9939x, sleeps 2001
```

That is 99.4 % of a processor consumed while `/proc/stat` reports 0.00 % on the
same core. Note the contrast with the per-task column, which stays near the real
value: the per-task counters follow the thread, while the tick-sampled per-CPU
busy counter never sees it because every sample lands inside the sleep window.

## Why the exaggerated usage is capped at 50%

This section explains the ceiling that the `too-much` program runs into. The
reported usage is **not** `busy / wall`. `/proc/stat`, and therefore `top`,
compute `busy / total` with `total = busy + idle`, and the two terms come from
the two independent accounting paths described earlier:

1. **`busy` is tick-sampled and saturates at one tick per tick.** At every
   sample `account_process_tick()` adds exactly one `TICK_NSEC` (4 ms at
   250 Hz) to whatever is current. If the busy loop intercepts every sample, the
   kernel charges `wall / HZ * TICK_NSEC = wall` of busy time. It cannot charge
   more than one tick per tick, so raw busy reaches **100 % of wall time** and
   stops there.

2. **`idle` is measured precisely and independently.** Because it uses the NOHZ
   accumulator, it records the real idle time of the core, including the very
   ticks that were charged as busy.

The same wall interval is counted in both places. Over a window `W` with real
duty cycle `d`:

```
busy_charged ≈ W            (saturated: one tick per tick)
idle         ≈ W − d·W      (precise NOHZ idle)

usage = W / (W + W − d·W) = 1 / (2 − d)
```

This is why intercepting every sample yields 50 % and not 100 %: the numerator
is maxed, but the denominator is nearly doubled by the separately-counted idle
term. A raw capture over 5 s (500 jiffies) makes it explicit:

```
busy  = 500 jiffies   ->  busy/wall  = 100%
idle  = 488 jiffies
total = 988 jiffies
reported usage = busy/total = 50.6%
```

The relation `usage = 1 / (2 − d)` matches measurement:

| busy / 1 ms | real usage `d` | reported per-CPU |
|---|---|---|
| 50 µs | 5 % | 51.8 % |
| 100 µs | 10 % | 53.2 % |
| 200 µs | 20 % | 55.9 % |
| 400 µs | 40 % | 63.3 % |
| 600 µs | 60 % | 72.1 % |
| 800 µs | 80 % | 84.0 % |
| 950 µs | 95 % | 96.2 % |

Consequences:

- As `d → 0`, `usage → 50 %`. This is the maximum *free* usage: a full wall's
  worth of busy charge against a full wall's worth of idle.
- `usage = 100 %` only when `d = 1`, i.e. the core is genuinely busy the whole
  time, so there is no idle left to count.
- The quantity worth maximizing is the exaggeration ratio,
  `reported / real = 1 / (d · (2 − d))`, which is largest at the smallest `d`
  that still intercepts every sample. That is exactly the equilibrium the
  optimizer converges to (500 ns per 1 ms).

## Caveats

- The effect and the achievable exaggeration depend on the kernel version and
  configuration, in particular `CONFIG_HZ`, `CONFIG_NO_HZ` and the cputime
  accounting options.
- The chosen core should be otherwise quiet; other activity on the core adds to
  the reported per-CPU usage and to the measured background.
- The demos are measurement artifacts, not real workloads. The reported values
  do not correspond to useful work.
