#include "FactoryModes.h"

namespace brainscape::plugin {

size_t FactoryCount() noexcept {
  size_t count = 0;
  FactoryPackages(&count);
  return count;
}

const FactoryPackage& Factory(size_t index) noexcept {
  size_t count = 0;
  return FactoryPackages(&count)[index];
}

int FindFactory(std::string_view id) noexcept {
  size_t                count = 0;
  const FactoryPackage* table = FactoryPackages(&count);
  for (size_t i = 0; i < count; ++i) {
    if (id == table[i].id) return static_cast<int>(i);
  }
  return -1;
}

const char* FamilyName(PresetFamily family) noexcept {
  switch (family) {
    case PresetFamily::Echoic: return "echoic";
    case PresetFamily::Reverie: return "reverie";
    case PresetFamily::Recall: return "recall";
    case PresetFamily::Misfire: return "misfire";
    case PresetFamily::None: break;
  }
  return "none";
}

std::vector<size_t> FactoryMenuOrder() {
  size_t                count = 0;
  const FactoryPackage* table = FactoryPackages(&count);
  std::vector<size_t>   order;
  order.reserve(count);
  const auto known = [](PresetFamily f) {
    for (PresetFamily k : kFamilyOrder) {
      if (k == f) return true;
    }
    return false;
  };
  for (PresetFamily f : kFamilyOrder) {
    for (size_t i = 0; i < count; ++i) {
      if (!table[i].reserve && table[i].family == f) order.push_back(i);
    }
  }
  for (size_t i = 0; i < count; ++i) {
    if (!table[i].reserve && !known(table[i].family)) order.push_back(i);
  }
  for (size_t i = 0; i < count; ++i) {
    if (table[i].reserve) order.push_back(i);
  }
  return order;
}

std::unique_ptr<PresetState> DecodeFactory(size_t index, PresetDiagnostic* diagnostic, PackageInfo* info,
                                           PresetMeta* meta) {
  if (index >= FactoryCount()) return nullptr;
  const FactoryPackage& f     = Factory(index);
  auto                  state = std::make_unique<PresetState>();
  if (!DecodePreset(f.bytes, f.size, state.get(), diagnostic, info, meta)) return nullptr;
  return state;
}

}  // namespace brainscape::plugin
