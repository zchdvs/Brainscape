// libFuzzer harness for the package decoder and validator (docs/design/mode-compiler.md §10.2),
// for the Linux Clang leg with AddressSanitizer and UndefinedBehaviorSanitizer:
//
//   cmake -DCMAKE_CXX_COMPILER=clang++ -DBRAINSCAPE_BUILD_LIBFUZZER=ON ...
//   brainscape_blob_libfuzzer -max_total_time=120 CORPUS_DIR
//
// Each input is decoded as it is and again with its hashes fixed (so mutations reach the
// section, STAT, MODE, CTRL and META rules), with this build's features and with every
// feature; every accepted package must re-encode to its own bytes, and ValidateMode runs on it.
// Seeds: blob_tool --write-fixtures, or the packages BlobSamples.cpp's Seeds() builds.
#include <cstdint>
#include <cstring>
#include <memory>

#include "../../src/blob/Blob.h"
#include "BlobSamples.h"

using namespace brainscape;

namespace {

void Check(const uint8_t* data, size_t size, PresetState* state, uint8_t* again) {
  for (const uint32_t features : {kSupportedModeFeatures, kModeFeatureAll}) {
    if (!blob::DecodePresetWith(data, size, state, nullptr, nullptr, nullptr, features)) continue;
    const size_t n = ReencodePackage(*state, data, size, 0, again, kMaxPackageBytes);
    if (n != size || std::memcmp(again, data, size) != 0) __builtin_trap();
    blob::ValidateModeWith(*state, features, nullptr);
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  static auto state = std::make_unique<PresetState>();
  static auto again = std::make_unique<uint8_t[]>(kMaxPackageBytes);
  Check(data, size, state.get(), again.get());
  blobtest::Bytes fixed(data, data + size);
  blobtest::Rehash(fixed);
  Check(fixed.data(), fixed.size(), state.get(), again.get());
  return 0;
}
