# CLOCK design probes

The two probes behind [clock.md](../../../docs/design/clock.md) §2.2, §3.3 and §6.3, as the design
pass ran them (evidence in its [record](../../../docs/design/reviews/clock-record.md) §2.6 and §2.7),
copied here unchanged by the tempo core's implementation (lane T1), as record §1.3 asks. Python 3,
exact integer arithmetic, no dependencies:

- [`grid.py`](grid.py): the grid-firing rule of §6.3 (a catch-up at every span start, then the
  positions after the phasor's tick), rendered for 30 s at 20, 97, 137.5, 140 and 300 BPM with grids
  of 3, 24 and 96 ticks and 25 random tempo changes and taps, at block sizes 1, 48, 441, 512 and two
  random sequences: every render equals block size 1, and at constant tempo every hit lands on
  ⌈k·P/K⌉. `python3 grid.py` prints the quarters fired in 60 s at 48-frame blocks (140, 138, 97,
  300, 20) and `all equal: True` (about 20 s).
- [`sums.py`](sums.py): the follower's incremental least-squares sums (add, remove, re-base onto a
  new oldest tick from the old sums) against direct sums over 20,000 ticks with dropouts, and the
  `int64_t` bounds of §3.3 at 384 kHz.

The production tests do not run these: `dsp/tests/test_tempo.cpp` and `test_tempo_rules.cpp` check
`TempoCore` against a C++ reference model written from the design (`dsp/tests/TempoReference.h`:
the phasor in closed form with exact 128-bit arithmetic, the grid frame by frame, the sums computed
directly, gaps applied at their deadlines) and against hand-worked cases, and
`brainscape_tempo_tool --check` repeats the reference comparison on every CI leg and under qemu-arm
for the Cortex-M7 with a committed digest.
