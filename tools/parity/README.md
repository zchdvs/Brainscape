# Parity evidence probes

These are the probe programs behind [determinism-profile.md](../../docs/design/determinism-profile.md)
and [companion-app.md](../../docs/design/companion-app.md), kept so every measured number in those
documents can be reproduced. They are **evidence, not production code**: nothing here is built by the
root CMake project, and apart from the block-split fix (`bugcheck/`) the engine changes they test have
not landed in `dsp/` yet. The review records
in [docs/design/reviews/](../../docs/design/reviews/) cite them.

The probes ran against `dsp/` at commit `e86e971`. Each modified engine copy is stored as a patch
against that tree rather than as a full copy.

The production parity tooling grown from them lives elsewhere: the test-signal generator in
`dsp/include/brainscape/TestSignal.h`, the golden-hash harness in `dsp/tests/golden/` (ctest
`golden_report`; `brainscape_golden --help`), the CI legs in `.github/workflows/parity.yml` and the
static audits and report comparison in `tools/ci/`. [`testsignal_ref.py`](testsignal_ref.py) here is
the separate Python implementation of the generator that derives the known-answer hashes in
`dsp/tests/test_testsignal.cpp` (`python3 tools/parity/testsignal_ref.py 480000`).

| Directory | What it shows |
| --- | --- |
| [`prototype/`](prototype/) | The determinism-profile prototype: in-tree math (`DetMath.h`) replacing every libm transcendental, plus `-ffp-contract=off`, giving one SHA-256 across 32 builds (MSVC, GCC 11/12/14, Clang 14, LTO variants, and Cortex-M7 code from arm-none-eabi-gcc 10.3/12.3 run under `qemu-arm -cpu cortex-m7`). Also the accuracy checker for the in-tree math and the explicit-FMA variant. |
| [`oracle/`](oracle/) | The emulated Cortex-M7 parity oracle: one harness compiled for x86 and, with the exact firmware flags, for the M7, run under qemu-arm user mode through a small syscall shim (`arm_sys.c`). |
| [`bugcheck/`](bugcheck/) | The block-split invariance checker that found the three freeze/onset-mark/far-rail defects, the verified fix (`fix.diff`, which includes the regression test), the alternative freeze semantics (`fixB-over-fix.diff`, "pin-eligible marks"), and the logs. |
| [`fp-semantics/`](fp-semantics/) | Floating-point semantics probes: the x86-FTZ vs Arm-FZ tininess boundary (`fpprobe.cpp`, `arm_boundary.cpp`), the whole-engine subnormal experiment (`denorm_engine.cpp`), contraction defaults per compiler, float-to-int casts, literal conversion, and the inline-function ODR hazard (`odr/`). |
| [`juce/`](juce/) | The float-normalisation round trip that rules out JUCE's normalised parameter path for exact preset values. |
| [`preset/`](preset/) | Exhaustive binary32 number-formatting check for canonical JSON (`probe_float.cpp`), and the restart/load-order/Spillover probes (`probe_state.cpp`, which needs a scratch engine copy with probe hooks). |
| [`review-numerics/`](review-numerics/) | Probes from the numerics review: Spillover as specified, DetMath domain edges, DAZ canonicalization, the write-ahead invariant counter. |

Probes for findings that depended on third-party source trees (libDaisy, TinyUSB, the Daisy bootloader,
JUCE) are not stored here; their method and results are written up in the review records.

## Recreating the engine copies

The build scripts expect sibling engine trees named `dsp_ref` (unmodified), `dsp_orig` (unmodified),
`dsp_det` (determinism profile) and `dsp_fma` (explicit-FMA variant). From the repository root, at
`e86e971` or a compatible tree:

```bash
cp -r dsp tools/parity/prototype/dsp_ref
cp -r dsp tools/parity/prototype/dsp_det && git apply --directory=tools/parity/prototype/dsp_det -p2 tools/parity/prototype/detmath-profile.patch
cp -r dsp tools/parity/prototype/dsp_fma && git apply --directory=tools/parity/prototype/dsp_fma -p2 tools/parity/prototype/explicit-fma-variant.patch
```

The oracle uses `oracle/dsp_orig` (unmodified) and `oracle/dsp_det` (`oracle/oracle-det.patch`, an
earlier, equivalent in-tree math variant), created the same way. The block-split fix
(`bugcheck/fix.diff`, then `fixB-over-fix.diff`) has landed in `dsp/`; the patches apply to a `dsp/`
tree at `e86e971`.
Do not commit the recreated trees.

## Running

- **Battery input.** Build and run `prototype/harness/gen_input.cpp` once to write the 30 s stereo
  input (`input.f32`, 11.5 MB, not stored here). Every build then reads the same bytes.
- **x86 builds.** `prototype/CMakeLists.txt` (MSVC variants, VS 2022 generator) and
  `prototype/linux_build_run.sh` (GCC/Clang, run inside a Linux container) build one battery
  executable per flag set and print a SHA-256 per preset. `prototype/summarize.py` groups the
  hash files; the results from the original runs are in `prototype/hashes/`.
- **Cortex-M7 builds.** `prototype/arm_build.sh` and `oracle/build_arm.sh` compile with the firmware
  flags (`-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -O3`). Run the ELF with
  `QEMU_CPU=cortex-m7` under qemu-arm user mode; Docker Desktop's built-in binfmt emulation was
  enough for the original runs.
- **Block-split checker.** `bugcheck/CMakeLists.txt` builds `bugcheck` against `../../../dsp`
  (override with `-DBUGCHECK_DSP_DIR=...` to test another tree; `dsp/` from `e86e971` reproduces the
  defects). `bugcheck matrix` runs all 32
  feature subsets; `bugcheck all` runs every named case. Logs from the original runs are in
  `bugcheck/results/`.

Always clamp each rendered block to `min(block, remaining)`: a fixed block size over a length that is
not a multiple of it overruns the output buffers and crashes later with a heap-corruption error.
