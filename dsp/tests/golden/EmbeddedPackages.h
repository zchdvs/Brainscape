#pragma once
#include <cstddef>
#include <cstdint>

// The corpus's committed packages compiled into the program (docs/design/mode-compiler.md
// §10.3), for a target without a file system: the Daisy Seed images (firmware/README.md) and
// brainscape_parity_stream, which renders as the parity image does. EmbedPackages.cmake
// generates the table at build time from presets/MANIFEST and the .bsp beside each document,
// byte for byte, so an embedded package cannot differ from its committed file. Linking the
// table (GoldenPackages.cmake) defines BRAINSCAPE_GOLDEN_EMBEDDED_PACKAGES, and LoadPackage
// (EventScript.h) reads it instead of presets/NAME.bsp.
namespace brainscape::golden {

struct EmbeddedPackage {
  const char*    name;   // NAME of presets/NAME.bsp
  const uint8_t* bytes;
  uint32_t       size;
};

// Every package presets/MANIFEST lists, in its order (by path); *count receives how many.
const EmbeddedPackage* EmbeddedPackages(size_t* count) noexcept;

}  // namespace brainscape::golden
