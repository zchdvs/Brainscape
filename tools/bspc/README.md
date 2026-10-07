# bspc — the preset compiler

`bspc` turns a preset document (JSON, schema 1) into a `.bsp` package and back
([mode-compiler.md](../../docs/design/mode-compiler.md) §8). It is a thin shell over the
`brainscape_compiler` library in [`compiler/`](../../compiler), which builds for desktop hosts
only. Compiling is a pure function of the document and the build's constants (§8.3): no clock,
user, locale or floating-point arithmetic, so every host writes the same bytes.

```bash
cmake -B build -DBRAINSCAPE_BUILD_TESTS=ON && cmake --build build --config Release --target bspc
build/tools/bspc/bspc compile engram.json        # writes engram.bsp
```

| Command | Does |
| --- | --- |
| `compile DOC.json [-o OUT.bsp]` | Compiles a document (default output: the document's name with `.bsp`). Errors E1–E12 stop it (§2.7). |
| `decompile PKG.bsp [--rebuild] [-o OUT.json]` | Prints the package's JSON section, or the document rebuilt from STAT, MODE, CTRL and META when it has none, its `JSON_STALE` flag is set, or `--rebuild` asks (editor data is then lost). |
| `fmt [--check] DOC.json...` | Rewrites documents in canonical form (§6.4); `--check` lists the files that differ and exits 1. The stamp is kept as written. |
| `verify PKG.bsp...` | The package decodes and validates, and its JSON section (unless stale) compiles to the same STAT and MODE. |
| `stamp [--check] FILE...` | Documents: rewrites `sound_rev` and `sound_hash` for this build (after a sound-revision bump). Packages: recompiles them from their JSON section. `--check` lists what is stale. |
| `lint [--factory] DOC.json...` | Lint findings L1–L9 (§2.7); `--factory` makes L4 and L7–L9 errors. |
| `diff A.bsp B.bsp` | The first differing field, by its JSON pointer (`/layers/0/size_ms: 120 -> 150`). |
| `derive [--solve] DOC.json...` | Rewrites each targeted leaf as its macro's value at the stored position (§3.5); `--solve` first sets each position from its first target's leaf. In place. |
| `roundtrip [--expect M] [--write-manifest M] DOC.json...` | The `bspc-roundtrip` checks (§8.3, §10.1): each document compiles (to its committed `.bsp`, when there is one), is canonical and stamped, decompiles to itself, rebuilds from its package without the JSON section, and its JSON section recompiles to the same bytes. Prints the sorted manifest: package hash, `sound_hash`, `control_hash` and path per line. |
| `migrate-session SESSION [--id ID] [--name NAME] [-o OUT.json]` | A BSWS v1 plugin session as a preset document (§4.4). |
| `version` | This build's sound revision, formats and supported mode features. |

Findings print as `file:line:column: error E4 at /layers/0/size_ms: message`, with the JSON
pointer of the value. Exit codes: 0 success, 1 findings or differences, 2 usage or I/O.

`render` (§8.2) arrives with the audition tooling in `tools/audition/` (lane E).

**What this build compiles.** Schema 1 names the whole vocabulary of the waves (§1.2); a
field this build cannot play is error E6, naming the feature and the wave that brings it. At
sound revision 1 only the default structure compiles: no onset source or mark positioning
(sound revision 2), no wave-1 to wave-3 fields, and the leaves of later waves only at their
defaults. Macros, macro positions and expression assignments compile.

**Tests.** `ctest` runs `compiler_unit` (the JSON grammar suite, every rule E1–E12, the
canonical form, packages, lint and derive, and two committed digests: 400 random documents'
packages and a 20,000-mutant reader fuzz) and `compiler_roundtrip`, which runs
`bspc roundtrip --expect MANIFEST` over `compiler/tests/data/*.json`. The manifest holds the
packages' hashes, not the packages: none is committed before sound revision 2.
