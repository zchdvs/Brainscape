# Daisy Seed Rev7, 2026-10-07: the captures

The device streams of the first two sessions on the owner's Daisy Seed Rev7, as the host tools
saved them, with session 1's bench report and its images' archive hashes (CRLF line ends;
`.gitattributes` keeps every byte). What they show, and what each settles in the designs, is in
[docs/design/reviews/rev7-silicon-record.md](../../../docs/design/reviews/rev7-silicon-record.md).
Every image reports the same board: "Daisy Seed 1.2 / rev7 (PCM3060; PD5 strap)", CPUID
`0x411FC271`, 480 MHz, booted from QSPI flash by the Daisy bootloader, libDaisy v9.0.0 and
arm-none-eabi-gcc 10.3.1, I- and D-cache on.

## Session 1: sound revision 1 (`session-1/`)

Every image was built from `a91aa8ad64c7` (a clean tree), engine archive
`4f4ddaa3583e46f29b84a008f5a80ad0938cacf05f2e961f70b1595ebf69da58`, the hooks archive
`da7b4f2e9b44aef70f52ab404b95f9d1015a9bd0996c462ed26c521cd1875a4f`.

| File | What it is | Image |
| --- | --- | --- |
| `parity-rev7-run.log` | Parity, `run`: the whole revision-1 corpus, `maxBlockSize` 512, clean FP environment | `brainscape_parity` |
| `parity-rev7-run-pedal.log` | Parity, `run pedal`: `maxBlockSize` 48 | `brainscape_parity` |
| `parity-rev7-run-hostile.log` | Parity, `run hostile`: FZ, DN and round toward zero in the caller's FPSCR | `brainscape_parity` |
| `bench-bench-ITCM.log` | The DWT pass, `run`, engine code in ITCM (the shipping placement) | `brainscape_bench` |
| `bench-bench-XIP.log` | The DWT pass, `run`, engine code executing in place from QSPI | `brainscape_bench_xip` |
| `bench-bench-ITCM-hooks.log` | The DWT pass, `run`, on the engine built with the FP guard's test hooks: FZ = 1 renders and the IDC/UFC flag census | `brainscape_bench_hooks` |
| `hello-bench-ITCM.json`, `hello-bench-XIP.json`, `hello-bench-ITCM-hooks.json` | Each bench image's `info` reply, saved on its own just before its run: build, board, FP and cache registers, memory map | the bench image named |
| `bench-all.md` | The report `bench_report.py` wrote from the three bench logs in the session (`--markdown`), which the silicon record quotes | — |
| `engine-archives.sha256` | The two archive hashes the images carry (each hello's `engineArchiveSha256` and `hooksArchiveSha256`), in the firmware build's `engine-archives.sha256` format, for `--expect-archive` | — |

An earlier run of `brainscape_bench_xip` was interrupted; its partial log is not kept.

Re-check them with the tools and golden file of the commit that built the images: this tree's
`parity_check.py` refuses the revision-1 stream format (`/2`), and its `golden.json` is revision
7's, against which the bench's golden-tail renders cannot match. Once:

```text
git worktree add --detach ../brainscape-a91aa8 a91aa8ad64c7
```

then, from this repository's root:

```text
python ../brainscape-a91aa8/tools/hil/parity_check.py --log firmware/records/rev7-2026-10-07/session-1/parity-rev7-run.log --run "run" --expect-archive 4f4ddaa3583e46f29b84a008f5a80ad0938cacf05f2e961f70b1595ebf69da58
python ../brainscape-a91aa8/tools/hil/parity_check.py --log firmware/records/rev7-2026-10-07/session-1/parity-rev7-run-pedal.log --run "run pedal" --expect-archive 4f4ddaa3583e46f29b84a008f5a80ad0938cacf05f2e961f70b1595ebf69da58
python ../brainscape-a91aa8/tools/hil/parity_check.py --log firmware/records/rev7-2026-10-07/session-1/parity-rev7-run-hostile.log --run "run hostile" --expect-archive 4f4ddaa3583e46f29b84a008f5a80ad0938cacf05f2e961f70b1595ebf69da58
python ../brainscape-a91aa8/tools/hil/bench_report.py --log firmware/records/rev7-2026-10-07/session-1/bench-bench-ITCM.log --log firmware/records/rev7-2026-10-07/session-1/bench-bench-XIP.log --log firmware/records/rev7-2026-10-07/session-1/bench-bench-ITCM-hooks.log --expect-archive firmware/records/rev7-2026-10-07/session-1/engine-archives.sha256 --markdown ../bench-all.md
```

Each parity check ends `VERDICT: PASS - 28 preset(s) match golden.json bit for bit (sound
revision 1, whole corpus)`, exit 0. The bench report checks all three runs' archives against
`engine-archives.sha256` and exits 0 with no run INCOMPLETE, and the `../bench-all.md` it writes
equals the committed `bench-all.md` byte for byte. Each hello file holds what the first line of
its bench log holds, pretty-printed, but for `uptimeMs` (taken about a second earlier).

## Session 2: sound revision 3 (`session-2/`)

The parity image built from `main` at `4090270` (its hello reports build `409027096f97`, a clean
tree), engine archive `1d6fe1dc41f02fd965783d70d8bfb63bd28a363b59d6915f54e60b4ae7401ced` (hooks
`023a9fa933fa9c0d…`), with the corpus's 18 packages compiled in.

| File | What it is | Image |
| --- | --- | --- |
| `parity-r3-run.log` | Parity, `run`: the whole revision-3 corpus, `maxBlockSize` 512, clean FP environment | `brainscape_parity` |
| `parity-r3-run-pedal.log` | Parity, `run pedal`: `maxBlockSize` 48 | `brainscape_parity` |
| `parity-r3-run-hostile.log` | Parity, `run hostile` | `brainscape_parity` |

Re-check them, as for session 1, with the tools and golden file of the commit that built the
image: this tree's `golden.json` is revision 7's, against which revision 3's renders cannot
match. Once:

```text
git worktree add --detach ../brainscape-409027 4090270
```

then, from this repository's root (the script reads the golden file and `MANIFEST` beside
itself, so these are `4090270`'s):

```text
python ../brainscape-409027/tools/hil/parity_check.py --log firmware/records/rev7-2026-10-07/session-2/parity-r3-run.log --run "run" --expect-archive 1d6fe1dc41f02fd965783d70d8bfb63bd28a363b59d6915f54e60b4ae7401ced
python ../brainscape-409027/tools/hil/parity_check.py --log firmware/records/rev7-2026-10-07/session-2/parity-r3-run-pedal.log --run "run pedal" --expect-archive 1d6fe1dc41f02fd965783d70d8bfb63bd28a363b59d6915f54e60b4ae7401ced
python ../brainscape-409027/tools/hil/parity_check.py --log firmware/records/rev7-2026-10-07/session-2/parity-r3-run-hostile.log --run "run hostile" --expect-archive 1d6fe1dc41f02fd965783d70d8bfb63bd28a363b59d6915f54e60b4ae7401ced
```

Each ends `VERDICT: PASS - 33 preset(s) match golden.json bit for bit, 18 package(s) match
MANIFEST (sound revision 3, whole corpus)`, exit 0.

These images link libDaisy's USB code (ST's SLA0044): the captures are data, but the images
that produced them must not be distributed (firmware/README.md §10).
