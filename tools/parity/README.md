# Parity evidence probes

These are the probe programs behind [determinism-profile.md](../../docs/design/determinism-profile.md)
and [companion-app.md](../../docs/design/companion-app.md), kept so every measured number in those
documents can be reproduced. They are **evidence, not production code**: nothing here is built by the
root CMake project. The engine changes they test have since landed in `dsp/` in their production form
(the build profile, in-tree math, the control-word guard and flush, the NaN-free boundary and the
block-split fix), so these probes describe the engine as it was, not as it is. The review records in
[docs/design/reviews/](../../docs/design/reviews/) cite them.

The probes ran against `dsp/` at commit `e86e971`, before that work: the layout they patch
(`dsp/include/brainscape/detail/`, `DenormalGuard.h`) no longer exists. Each modified engine copy is
stored as a patch against that tree rather than as a full copy; recreate them from a checkout of
`e86e971`.

The production parity tooling grown from them lives elsewhere: the test-signal generator in
`dsp/include/brainscape/TestSignal.h`, the golden-hash harness in `dsp/tests/golden/` with the
golden file it checks (ctest `golden_check`; `brainscape_golden --help`), the CI legs in
`.github/workflows/parity.yml` and `sound-rev.yml`, and the static audits, report comparison and
sound-revision gate in `tools/ci/`. The contraction-on negative control builds through
the test-only `-DBRAINSCAPE_FP_NEGATIVE_CONTROL=ON` (`cmake/BrainscapeFpProfile.cmake`), under which the
harness only reports. The option is never cached, so it lasts one configure, and it refuses plugin and
firmware builds: give it its own build directory. [`testsignal_ref.py`](testsignal_ref.py) here is
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
| [`modes/`](modes/) | The mode-compiler design's probes ([mode-compiler.md](../../docs/design/mode-compiler.md), evidence in its [record](../../docs/design/reviews/mode-compiler-record.md)): the parameter-routing bug, the factory-mode curation renders, the exact binary32 number code, the package sketch with its fuzzers, macro-curve cost, layout hazards, and the reviews' checks. See [Mode-compiler probes](#mode-compiler-probes-modes). |

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
(`bugcheck/fix.diff`, then `fixB-over-fix.diff`) has landed in `dsp/` (parts A, B and C with
pin-eligible marks); the patches apply to a `dsp/` tree at `e86e971`.
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
  defects). Today's `dsp/` includes `../cmake/BrainscapeFpProfile.cmake`, so the directory you point
  at needs the repository's `cmake/` beside it. `bugcheck matrix` runs all 32 feature subsets;
  `bugcheck all` runs every named case; `bugcheck hashes` prints one hash per case to compare two
  trees. Logs from the original runs are in `bugcheck/results/`; against today's tree both modes
  report 0 broken cases (Release, and `all` in Debug with the write-ahead assertion live).

Always clamp each rendered block to `min(block, remaining)`: a fixed block size over a length that is
not a multiple of it overruns the output buffers and crashes later with a heap-corruption error.

## Mode-compiler probes (`modes/`)

The probes behind [mode-compiler.md](../../docs/design/mode-compiler.md), copied from the design
session's scratch directory by mode-compiler lane G so the record's measured numbers can be
reproduced. Each directory keeps its scratch name, so the paths the
[evidence record](../../docs/design/reviews/mode-compiler-record.md) cites (§1.3, "`tech/bsnum.h`",
"`review-implementation-tooling/exact_check.py`") resolve under `modes/`. Sources and run scripts
only: their outputs, binaries and build trees are not kept (the record quotes the results), nor is
the Microcosm manual text the curation read. The probes ran against `dsp/` at `42773a2`, before the
mode-compiler lanes; the number code and the package sketch grew into `compiler/src/Number.*` and
`dsp/src/blob/`, whose own tests now carry their checks, so these describe the prototypes, not the
built code. The only edits are paths: absolute paths became relative to the repository, and the
gate probe also prints what today's package rule says.

| Directory | Record | What it shows |
| --- | --- | --- |
| [`inventory_probe/`](modes/inventory_probe/) | §2.1 | A change to only ID 27 or 28 by an event is lost (the routing bug); build `probe.cpp` against `dsp/` (MSVC: `dsp/include`, `dsp/src` and its sources). |
| [`probe/`](modes/probe/) | §2.2 | The curation probe: 14 factory-mode recipes (`recipes.inc`; `recipes_draft.inc` is the first pass) rendered over three integer-generated scores, with level, tail and periodicity figures. `cmake -S tools/parity/modes/probe -B build/mode_probe` (`MODE_PROBE_DSP_DIR` picks another tree). |
| [`tech/`](modes/tech/) | §2.3–§2.7 | `bsnum.h` and `numtest.cpp`: the exact binary32 reader and shortest writer with the exhaustive, halfway, random and edge checks against `<charconv>`; `blob_format.h`, `blob_runtime.cpp`, `blob_compile.cpp`: the fixed-capacity package sketch, its decoder, validator and mutation fuzzer; `blob_libfuzzer.cpp`; `macro_cost.cpp` (macro-curve cost, `mca_run.sh` for llvm-mca); `hazard.cpp` (struct layout across targets), `chkstk.cpp` (MSVC's `__chkstk`), `jsonh_probe.cpp` (what the golden harness's JSON reader accepts). `batch.sh` runs `linux_run.sh` in the GCC 11, GCC 14 and Clang 14 containers (`/w` is this directory, `/b` the repository); `arm_run.sh`, `arm_fp.sh` and the fuzz scripts run inside the GCC 11 image; `msvc.bat` and `dumpsyms.bat` are the MSVC wrappers. |
| [`review-engine-determinism/`](modes/review-engine-determinism/) | §2.8 | `keyalias.cpp`: draft v1's key extension aliasing at fixed frame offsets; `gate_probe.py`: the gate at `42773a2` missed a changed `soundHash`, the hole the package rule closes. |
| [`review-implementation-tooling/`](modes/review-implementation-tooling/) | §2.3, §2.8 | `exact_check.py` and `js_probe.js`: the two double-rounding values and what JavaScript prints; `audit_probe_fp.cpp` and `audit_probe_int.cpp` with `run.bat`: MSVC imports nothing that names its float conversions (why the compiler audit's import check runs on GCC and Clang only); `comdat_probe.cpp` with `run2.bat`: `kParamTable` as COMDAT data in a `bsc` object; `numtest-threads.patch`: the thread-count variant of `tech/numtest.cpp` that showed its hashes depend on the core count. |
| [`review-product-microcosm/`](modes/review-product-microcosm/) | §2.2, §2.8 | `engram_check.py` (Engram's FIFO drift, macro values, feedback energy) and `outdelta.py` (output against dry levels). |
| [`revise/`](modes/revise/) | §2.6 | `evalmacro.cpp` with `build.bat`: the evaluator's values on the Engram example, against `dsp/src/DetMath.cpp`. |
| [`mixlaw/`](modes/mixlaw/) | §2.2 | Added at sound revision 3, not from the design session, and built against this repository's `dsp/`: the first set's recipes re-measured under the Mix law, engaged against bypass in K-weighted loudness (BS.1770-4) and RMS, with peaks, over the curation probe's scores and the Plucks, Strums and SoftNotes vectors; it also checks that the engine's render equals the law's composition of dry and wet bit for bit. `cmake -S tools/parity/modes/mixlaw -B build/mixlaw`. |

The production checks grown from these: `compiler/tests/` (the number code's hashed sets, the JSON
grammar, the property and reader-fuzz digests), `dsp/tests/blob/` (the mutation fuzzer with its
committed digest, the libFuzzer harness, the frozen fixtures), and in CI `bspc-roundtrip`,
`blob-libfuzzer`, the compiler audit (`tools/ci/audit_compiler.py`) and the sound-revision gate's
package rule (`.github/workflows/parity.yml`, `nightly.yml`, `sound-rev.yml`).
