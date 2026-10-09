#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "brainscape/Preset.h"
#include "brainscape/PresetState.h"

namespace brainscape::plugin {

// The factory set (firmware/factory: MANIFEST, the schema-1 documents and the .bsp packages
// compiled beside them, mode-compiler.md §11.1), embedded in the plugin at build time by
// plugin/EmbedFactory.cmake, so the editor's Modes menu plays every mode without its file. One
// entry per MANIFEST line, the four of reserve/ included.
struct FactoryPackage {
  const char*    path;         // MANIFEST's document path: "lull.json", "reserve/runaway.json"
  const char*    id;           // the document's id: "factory.lull"
  const char*    name;         // its display name, UTF-8: "Lull"
  PresetFamily   family;
  bool           reserve;      // one of reserve/'s, kept for a mode the listening pass drops
  const uint8_t* bytes;        // the committed .bsp, byte for byte
  size_t         size;
  const char*    packageHash;  // MANIFEST's hashes of the package, 64 lowercase hex digits each
  const char*    soundHash;
  const char*    controlHash;
};

// Every package MANIFEST lists, in its order (generated: FactoryPackages.cpp in the build tree).
const FactoryPackage* FactoryPackages(size_t* count) noexcept;

size_t                FactoryCount() noexcept;
const FactoryPackage& Factory(size_t index) noexcept;  // index < FactoryCount()
// The package of a document id ("factory.lull"), or -1.
int FindFactory(std::string_view id) noexcept;

// A family's name as documents write it ("echoic"; "none" for PresetFamily::None).
const char* FamilyName(PresetFamily family) noexcept;
// The menu's family order: the order of firmware/factory/README.md's table.
inline constexpr PresetFamily kFamilyOrder[] = {PresetFamily::Echoic, PresetFamily::Reverie,
                                                PresetFamily::Recall, PresetFamily::Misfire};

// The Modes menu's order: the set's modes by family in kFamilyOrder, each family in MANIFEST
// order, then any of another family; the reserves after them, in MANIFEST order.
std::vector<size_t> FactoryMenuOrder();

// The package decoded as DecodePreset reads it, with its header and META; null (and the
// diagnostic) when it does not decode. Message thread: allocates the state.
std::unique_ptr<PresetState> DecodeFactory(size_t index, PresetDiagnostic* diagnostic = nullptr,
                                           PackageInfo* info = nullptr, PresetMeta* meta = nullptr);

}  // namespace brainscape::plugin
