# Frozen package fixtures

Packages that every later build must keep reading the same way
([mode-compiler.md](../../../../../docs/design/mode-compiler.md) §10.3): never re-stamped,
never rebuilt by `bspc`, exempt from `bspc-roundtrip`. Their bytes are pinned by SHA-256 in
[`dsp/tests/blob/Fixtures.cpp`](../../../blob/Fixtures.cpp), which also holds each one's verdict
on this build and why; `brainscape_blob_tool --fixtures` (host and emulated Cortex-M7) and the
`[fixtures]` unit test check them. A verdict a later build changes on purpose (a feature it now
supports, a retired leaf) is updated in the pull request that changes it, saying so; the bytes
never change. `brainscape_blob_tool --write-fixtures DIR` shows how each was made and refuses to
overwrite one.

| File | Verdict at sound revisions 2 and later | What it pins |
|---|---|---|
| `r1-default-mode.bsp` | decodes, loads inexact | a revision-1 package: every r1 leaf, the default mode, CTRL with an expression assignment, META. It loaded exact at r1; since r2 retired IDs 27 and 28 into mode structure, its leaves for them are unknown, so the load is inexact (so is every fixture below built from it) |
| `r2-onset-marks.bsp` | decodes, loads exact | a revision-2 package: every r2 leaf (IDs 1-26), onsets on marks, the default macros with Space on the reverb only, CTRL with expression assignments on a macro and a leaf, META. Loads exact, with no missing ID, on every later build: a leaf a later revision adds has a later `sinceRev` and is not missing from it (§7.3) |
| `unknown-sections.bsp` | decodes | two sections this build does not know, after MODE and after META: skipped, and carried byte for byte and in place by a re-encode |
| `unknown-chunk.bsp` | `UnsupportedFeature`, `ZZZZ` | a MODE chunk this build does not know is refused and named, never skipped |
| `unknown-feature-bit.bsp` | `UnsupportedFeature`, bit 31 | a feature bit no build assigns |
| `future-pitch-set.bsp` | `UnsupportedFeature`, pitch sets (revisions 2-4); decodes and validates since revision 5, loads inexact | a wave-1 package (the set {0, +12}), built from the revision-1 recipe; sound revision 5 plays pitch sets (mode-compiler.md §7.5 R10) |
| `blob-format-2.bsp` | `BlobFormat` | a newer STAT/MODE/CTRL layout |
| `package-format-2.bsp` | `PackageFormat` | a newer container |
| `future-sound-rev.bsp` | decodes | compiled at sound revision 1000 (§7.3: the load counts it as this build's revision) |
| `newer-schema.bsp` | decodes | a document schema newer than this build's: the package layer does not read it |
| `factory-json-stale.bsp` | decodes | the FACTORY and JSON_STALE flags and a JSON section, carried |
| `unknown-flag.bsp` | `HeaderFlags`, bit 2 | an unassigned header flag |
| `no-ctrl.bsp` | decodes | no CTRL section (`control.present` 0) |
| `w1-leaf-macro-target.bsp` | decodes; `ValidateMode`: `UnsupportedTarget`, ID 30 (revisions 2-5); validates since revision 6, loads inexact | a macro target on `layer0.decay_ms`, a wave-1 leaf (a Reserved row until sound revision 6 made it a Leaf, mode-compiler.md §7.5 R11): newer content, named, not a corrupt package |
| `w1-leaf-expression.bsp` | `UnsupportedTarget`, ID 30 (revisions 2-5); decodes and validates since revision 6, loads inexact | an expression assignment on the same leaf, which CTRL's rules refused at decode, named, until it became a Leaf row |
| `w3-leaf-macro-target.bsp` | decodes; `ValidateMode`: `UnsupportedTarget`, ID 32 (since revision 7, when it was added); its verdict changes in W3 | a macro target on `layer0.level_db` (-12 to 0 dB), a wave-3 leaf and a Reserved row: the role `w1-leaf-macro-target.bsp` held until revision 6 |
| `w3-leaf-expression.bsp` | `UnsupportedTarget`, ID 32 (since revision 7); its verdict changes in W3 | an expression assignment on the same leaf, refused at decode, named: the role `w1-leaf-expression.bsp` held until revision 6 |
| `w2-step-table.bsp` | `UnsupportedFeature`, step tables (since revision 7); its verdict changes in W2 | a wave-2 package (four steps, shuffled), built from the revision-1 recipe: the role `future-pitch-set.bsp` held until revision 5 |

The recipes in `Fixtures.cpp` spell out each revision's leaves rather than reading the build's
table, so `--write-fixtures` and the `[fixtures]` test keep rebuilding the committed bytes.
