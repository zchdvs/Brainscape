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

namespace {

// The menu's order, computed once (a function-local static: thread-safe, and any thread may ask).
const std::vector<size_t>& MenuOrder() {
  static const std::vector<size_t> order = FactoryMenuOrder();
  return order;
}

}  // namespace

int ProgramCount() noexcept { return static_cast<int>(FactoryCount()) + 2; }

int CustomProgram() noexcept { return ProgramCount() - 1; }

int FactoryOfProgram(int program) {
  const std::vector<size_t>& order = MenuOrder();
  if (program < 1 || static_cast<size_t>(program) > order.size()) return -1;
  return static_cast<int>(order[static_cast<size_t>(program) - 1]);
}

int ProgramOfFactory(int index) {
  const std::vector<size_t>& order = MenuOrder();
  for (size_t k = 0; k < order.size(); ++k) {
    if (static_cast<int>(order[k]) == index) return static_cast<int>(k) + 1;
  }
  return -1;
}

std::string ProgramName(int program) {
  if (program == kDefaultProgram) return "Default mode";
  if (program == CustomProgram()) return "Custom";
  const int index = FactoryOfProgram(program);
  if (index < 0) return {};
  const FactoryPackage& f = Factory(static_cast<size_t>(index));
  if (f.reserve) return std::string("Reserve: ") + f.name;
  if (f.family == PresetFamily::None) return f.name;
  std::string family = FamilyName(f.family);  // "echoic" -> "Echoic"
  if (!family.empty() && family[0] >= 'a' && family[0] <= 'z') family[0] = static_cast<char>(family[0] - 'a' + 'A');
  return family + ": " + f.name;
}

}  // namespace brainscape::plugin
