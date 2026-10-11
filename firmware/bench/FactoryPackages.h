#pragma once
#include <cstdint>

// Factory packages the bench configurations play (docs/design/cpu-budget.md §8.2: Murmuration at
// Activity max), compiled in from firmware/factory/NAME.bsp byte for byte at build time
// (firmware/cmake/EmbedFile.cmake): the images have no file system, and the host tool renders the
// same bytes.
namespace brainscape::fw::bench {

extern const uint8_t  kFactoryMurmuration[];
extern const uint32_t kFactoryMurmurationSize;

}  // namespace brainscape::fw::bench
