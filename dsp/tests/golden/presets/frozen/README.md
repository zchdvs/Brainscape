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

| File | Verdict at sound revision 1 | What it pins |
|---|---|---|
| `r1-default-mode.bsp` | decodes, loads exact | a revision-1 package: every r1 leaf, the default mode, CTRL with an expression assignment, META. At r2 it still decodes, but its leaves 27 and 28 are retired IDs, so the load is inexact |
| `unknown-sections.bsp` | decodes | two sections this build does not know, after MODE and after META: skipped, and carried byte for byte and in place by a re-encode |
| `unknown-chunk.bsp` | `UnsupportedFeature`, `ZZZZ` | a MODE chunk this build does not know is refused and named, never skipped |
| `unknown-feature-bit.bsp` | `UnsupportedFeature`, bit 31 | a feature bit no build assigns |
| `future-pitch-set.bsp` | `UnsupportedFeature`, pitch sets | a wave-1 package (the set {0, +12}); decodes once W1 supports pitch sets |
| `blob-format-2.bsp` | `BlobFormat` | a newer STAT/MODE/CTRL layout |
| `package-format-2.bsp` | `PackageFormat` | a newer container |
| `future-sound-rev.bsp` | decodes | compiled at sound revision 1000 (§7.3: the load counts it as this build's revision) |
| `newer-schema.bsp` | decodes | a document schema newer than this build's: the package layer does not read it |
| `factory-json-stale.bsp` | decodes | the FACTORY and JSON_STALE flags and a JSON section, carried |
| `unknown-flag.bsp` | `HeaderFlags`, bit 2 | an unassigned header flag |
| `no-ctrl.bsp` | decodes | no CTRL section (`control.present` 0) |

Lane C adds the sound-revision-2 package that must load exact, with no missing ID, on every
later build (the `sinceRev` rule).
