#include "detail/FpProfilePrivate.h"

#include "brainscape/SoundRevision.h"

// Compiled into brainscape_dsp, so the macros below describe the toolchain and flags that
// built the engine, never those of a consumer. dsp/CMakeLists.txt defines the flag text
// and its hash; a build outside CMake reports them as unknown.
#ifndef BRAINSCAPE_FP_FLAGS
#define BRAINSCAPE_FP_FLAGS "unknown"
#endif
#ifndef BRAINSCAPE_FP_FLAGS_HASH
#define BRAINSCAPE_FP_FLAGS_HASH "unknown"
#endif

#define BRAINSCAPE_STRINGIFY2(x) #x
#define BRAINSCAPE_STRINGIFY(x)  BRAINSCAPE_STRINGIFY2(x)

#if defined(__clang__) && defined(__apple_build_version__)
#define BRAINSCAPE_COMPILER "appleclang"
#define BRAINSCAPE_COMPILER_VERSION __clang_version__
#elif defined(__clang__)
#define BRAINSCAPE_COMPILER "clang"
#define BRAINSCAPE_COMPILER_VERSION __clang_version__
#elif defined(__GNUC__)
#define BRAINSCAPE_COMPILER "gcc"
#define BRAINSCAPE_COMPILER_VERSION __VERSION__
#elif defined(_MSC_VER)
#define BRAINSCAPE_COMPILER "msvc"
#define BRAINSCAPE_COMPILER_VERSION \
  BRAINSCAPE_STRINGIFY(_MSC_FULL_VER) "." BRAINSCAPE_STRINGIFY(_MSC_BUILD)
#else
#define BRAINSCAPE_COMPILER "unknown"
#define BRAINSCAPE_COMPILER_VERSION "unknown"
#endif

#if defined(__x86_64__) || defined(_M_X64)
#define BRAINSCAPE_ARCH "x86_64"
#elif defined(__aarch64__)
#define BRAINSCAPE_ARCH "aarch64"
#elif defined(__arm__) && defined(__ARM_ARCH_7EM__)
#define BRAINSCAPE_ARCH "armv7e-m"
#elif defined(__arm__)
#define BRAINSCAPE_ARCH "arm"
#else
#define BRAINSCAPE_ARCH "unknown"
#endif

#if defined(_WIN32)
#define BRAINSCAPE_SYSTEM "windows"
#elif defined(__APPLE__)
#define BRAINSCAPE_SYSTEM "darwin"
#elif defined(__linux__)
#define BRAINSCAPE_SYSTEM "linux"
#else
#define BRAINSCAPE_SYSTEM "none"
#endif

namespace brainscape {

const ToolchainId& BuildToolchain() noexcept {
  static constexpr ToolchainId kId{BRAINSCAPE_COMPILER, BRAINSCAPE_COMPILER_VERSION,
                                   BRAINSCAPE_ARCH "-" BRAINSCAPE_SYSTEM, BRAINSCAPE_FP_FLAGS,
                                   BRAINSCAPE_FP_FLAGS_HASH};
  return kId;
}

}  // namespace brainscape
