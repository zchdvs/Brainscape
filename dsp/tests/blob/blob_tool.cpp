// The package checks that run on every leg, the emulated Cortex-M7 included, whose 32-bit
// size_t the host legs miss (docs/design/mode-compiler.md §10.2, §10.3):
//
//   blob_tool --fuzz [N] [--seed S]    the deterministic mutation fuzzer; with the committed
//                                      N and seed its digest must be kFuzzDigest, so host and
//                                      M7 accept and reject the same inputs
//   blob_tool --fixtures DIR           the frozen fixtures' bytes and verdicts
//   blob_tool --write-fixtures DIR     writes the fixtures that do not exist yet (never
//                                      overwrites: the committed files are the reference)
//
// No exceptions or RTTI, so it builds for the M7 like the golden harness.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "BlobSamples.h"

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <stdlib.h>
#endif

using namespace brainscape;
using namespace brainscape::blobtest;

namespace {

bool ReadFile(const std::string& path, Bytes* out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return false;
  out->clear();
  uint8_t buf[4096];
  size_t  n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out->insert(out->end(), buf, buf + n);
  std::fclose(f);
  return true;
}

int RunFuzz(uint64_t iterations, uint64_t seed) {
  using Clock               = std::chrono::steady_clock;
  const auto        t0     = Clock::now();
  const FuzzResult  r      = blobtest::Fuzz(iterations, seed);
  const double      s      = std::chrono::duration<double>(Clock::now() - t0).count();
  const std::string digest = Hex(r.digest, 32);
  std::printf("blob fuzz: %llu iterations (seed %llu)",
              static_cast<unsigned long long>(r.iterations),
              static_cast<unsigned long long>(seed));
#if !defined(__arm__)  // the M7 harness under qemu-arm has no clock
  std::printf(" in %.1f s", s);
#else
  (void)s;
#endif
  std::printf("\n");
  std::printf("  %llu structural\n", static_cast<unsigned long long>(r.structural));
  std::printf("  accepted %llu (this build), %llu (every feature)\n",
              static_cast<unsigned long long>(r.accepted),
              static_cast<unsigned long long>(r.acceptedAll));
  std::printf("  ValidateMode-clean %llu (this build), %llu (every feature)\n",
              static_cast<unsigned long long>(r.validatedThisBuild),
              static_cast<unsigned long long>(r.validated));
  std::printf("  re-encode mismatches %llu\n",
              static_cast<unsigned long long>(r.reencodeMismatches));
  unsigned decodeCodes = 0, validateCodes = 0;
  std::printf("  DecodePreset (this build):\n");
  for (size_t e = 0; e < static_cast<size_t>(PresetError::kCount); ++e) {
    if (r.histogram[e] != 0) {
      decodeCodes += e != 0 ? 1u : 0u;
      std::printf("    %-20s %llu\n", PresetErrorName(static_cast<PresetError>(e)),
                  static_cast<unsigned long long>(r.histogram[e]));
    }
  }
  std::printf("  ValidateMode (every feature):\n");
  for (size_t e = 0; e < static_cast<size_t>(PresetError::kCount); ++e) {
    if (r.validateHistogram[e] != 0) {
      validateCodes += e != 0 ? 1u : 0u;
      std::printf("    %-20s %llu\n", PresetErrorName(static_cast<PresetError>(e)),
                  static_cast<unsigned long long>(r.validateHistogram[e]));
    }
  }
  std::printf("  codes reached: %u by DecodePreset, %u by ValidateMode\n", decodeCodes,
              validateCodes);
  std::printf("  verdict digest %s\n", digest.c_str());
  int rc = r.reencodeMismatches == 0 ? 0 : 1;
  if (iterations == kFuzzIterations && seed == kFuzzSeed) {
    const bool same = digest == kFuzzDigest;
    std::printf("  committed digest %s: %s\n", kFuzzDigest, same ? "match" : "MISMATCH");
    if (!same) rc = 1;
  }
  return rc;
}

int Fixtures(const std::string& dir, bool write) {
  int failures = 0;
  for (size_t i = 0; i < kFixtureCount; ++i) {
    const Fixture&    f    = kFixtures[i];
    const std::string path = dir + "/" + f.file;
    Bytes             bytes;
    if (write) {
      if (ReadFile(path, &bytes)) {
        std::printf("  %-28s exists, kept\n", f.file);
        continue;
      }
      bytes   = MakeFixture(i);
      FILE* o = std::fopen(path.c_str(), "wb");
      if (o == nullptr || std::fwrite(bytes.data(), 1, bytes.size(), o) != bytes.size()) {
        std::printf("  %-28s cannot write\n", f.file);
        ++failures;
      }
      if (o != nullptr) std::fclose(o);
    } else if (!ReadFile(path, &bytes)) {
      std::printf("  %-28s missing\n", f.file);
      ++failures;
      continue;
    }
    const std::string why = CheckFixture(f, bytes);
    std::printf("  %-28s %5zu bytes  %s\n", f.file, bytes.size(), why.empty() ? "ok" : why.c_str());
    if (!why.empty()) ++failures;
  }
  std::printf("blob fixtures: %zu checked, %d failed\n", kFixtureCount, failures);
  return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_MSC_VER)
  // MSVC's debug CRT answers a failed assertion with a dialog, which hangs ctest; report on
  // stderr and exit instead (as test_main.cpp does).
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  for (int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
    _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
  }
#endif
  const std::string mode = argc > 1 ? argv[1] : "";
  if (mode == "--fuzz") {
    uint64_t iterations = kFuzzIterations, seed = kFuzzSeed;
    for (int i = 2; i < argc; ++i) {
      if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
        seed = std::strtoull(argv[++i], nullptr, 10);
      } else {
        iterations = std::strtoull(argv[i], nullptr, 10);
      }
    }
    return RunFuzz(iterations, seed);
  }
  if ((mode == "--fixtures" || mode == "--write-fixtures") && argc > 2) {
    return Fixtures(argv[2], mode == "--write-fixtures");
  }
  std::printf("usage: blob_tool --fuzz [N] [--seed S] | --fixtures DIR | --write-fixtures DIR\n");
  return 2;
}
