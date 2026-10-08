# bspc — the preset compiler

`bspc` turns a preset document (JSON, schema 1) into a `.bsp` package and back
([mode-compiler.md](../../docs/design/mode-compiler.md) §8). It is a thin shell over the
`brainscape_compiler` library in [`compiler/`](../../compiler), which builds for desktop hosts
only. Compiling is a pure function of the document and the build's constants (§8.3): no clock,
user, locale or floating-point arithmetic, so every host writes the same bytes.

```bash
cmake -B build && cmake --build build --config Release --target bspc
# Visual Studio and Xcode (multi-config) put it in build/tools/bspc/Release/; Ninja and
# Makefiles in build/tools/bspc/.
build/tools/bspc/Release/bspc compile compiler/tests/data/engram.json   # writes engram.bsp
```

File arguments are taken as written, except on Windows, where cmd and PowerShell pass a wildcard
through unexpanded: there `bspc` expands `*` and `?` in an argument's last component itself, to
the matching files in sorted order (case ignored, dot files only for a pattern that starts with a
dot); a pattern that matches nothing stays as written and fails to read, and after `--` nothing
is expanded.

| Command | Does |
| --- | --- |
| `compile DOC.json [-o OUT.bsp]` | Compiles a document (default output: the document's name with `.bsp`). Errors E1–E12 stop it (§2.7). |
| `decompile PKG.bsp [--rebuild] [-o OUT.json]` | Prints the package's JSON section, or the document rebuilt from STAT, MODE, CTRL and META when it has none, its `JSON_STALE` flag is set, or `--rebuild` asks (editor data is then lost). |
| `fmt [--check] DOC.json...` | Rewrites documents in canonical form (§6.4); `--check` lists the files that differ and exits 1. The stamp is kept as written. |
| `verify PKG.bsp...` | The package decodes and validates, and its JSON section (unless stale) compiles to the same STAT and MODE. |
| `stamp [--check] FILE...` | Documents: rewrites `sound_rev` and `sound_hash` for this build. Needed after any change to what the preset plays (a leaf, `derive`, the structure) and after a sound-revision bump. Packages: recompiles them from their JSON section. `--check` lists what is stale. |
| `lint [--factory] DOC.json...` | Lint findings L1–L9 (§2.7), as warnings; `--factory` makes L4 and L7–L9 errors. |
| `diff A.bsp B.bsp` | The first differing field: a header format or revision, else the document's fields by JSON pointer (`/layers/0/size_ms: 120 -> 150`), those that play or control the sound before the id, name, META and display names; then the header flags, which follow from the id. |
| `derive [--solve] DOC.json...` | Rewrites each targeted leaf as its macro's value at the stored position (§3.5); `--solve` first sets each position from its first target's leaf, keeping the stored position when it already lands as near as any. In place; says when the stamp has gone stale. |
| `roundtrip [--expect M] [--write-manifest M] DOC.json...` | The `bspc-roundtrip` checks (§8.3, §10.1): each document compiles (to its committed `.bsp`, when there is one), is canonical and stamped, decompiles to itself, rebuilds from its package without the JSON section, and its JSON section recompiles to the same bytes. Prints the sorted manifest: package hash, `sound_hash`, `control_hash` and path per line; with `--expect`, names each document whose hashes differ. |
| `migrate-session SESSION [--id ID] [--name NAME] [-o OUT.json]` | A BSWS v1 plugin session as a preset document (§4.4). |
| `render [--script S0,...\|all] [--class attack\|pad] [--declarations AUDITION.md] [--metrics] [--no-wav] [-o DIR] FILE...` | The audition scripts S0–S11 (§11.3) for each document or package, the files given forming the set S10 and S11 visit: 16-bit WAVs, a recipe per render with its float32 hashes, an index per preset and, with `--metrics`, the objective pre-screen (exit 1 when a check fails). See [`tools/audition`](../audition/README.md). |
| `version` | This build's sound revision, formats and supported mode features. |

Each command takes only the options listed; any other option (`fmt --chek`, `fmt -o`) is
refused with exit 2 before a file is read or written, so a misspelled gate never passes or
rewrites what it checks. `--` ends the options.

Findings print as `file:line:column: error E4 at /layers/0/size_ms: message`, with the JSON
pointer of the value; leaves are named as schema 1 names them (`layer0.size_ms`), the form
macro targets and `editor.detached` take. Exit codes: 0 success (lint warnings included), 1
errors or differences, 2 usage or I/O.

## Writing a preset

1. Write the JSON (§2; [`compiler/tests/data/`](../../compiler/tests/data) has examples).
2. `bspc fmt DOC.json`: canonical form.
3. `bspc derive DOC.json`, after setting macro positions; or `bspc derive --solve DOC.json`,
   after setting leaves.
4. `bspc stamp DOC.json`: needed after any change to the sound; `fmt --check` passes with a
   stale stamp, `stamp --check` and `roundtrip` do not.
5. `bspc lint --factory DOC.json`: what CI requires of factory presets.
6. `bspc compile DOC.json`, or `bspc roundtrip DOC.json` for every check at once.

**What this build compiles.** Schema 1 names the whole vocabulary of the waves (§1.2); a
field this build cannot play is error E6, naming the feature and the wave that brings it. Since
sound revision 2 the default structure compiles, with the onset source and mark positioning
(rows 27 and 28 until then): no wave-1 to wave-3 fields, and the leaves of later waves only at
their defaults. Macros, macro positions and expression assignments compile.

**Tests.** `ctest` runs `compiler_unit` (the JSON grammar suite, every rule E1–E12, the
canonical form, packages, lint and derive, and two committed digests: 400 random documents'
packages and a 20,000-mutant reader fuzz), `compiler_roundtrip`, which runs
`bspc roundtrip --expect MANIFEST` over `compiler/tests/data/*.json`, and `compiler_bspc_cli`
(`compiler/tests/bspc_cli.cmake`: refused options, the authoring sequence's messages and
non-ASCII file names). The manifest holds the packages' hashes, not the packages; the golden
corpus commits its packages beside its documents (`dsp/tests/golden/presets/`, since sound
revision 2).
