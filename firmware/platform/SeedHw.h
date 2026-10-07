#pragma once
// The libDaisy board object, for the images that drive the Seed's audio directly (the live
// image). Platform.h stays free of libDaisy headers.
#include "daisy_seed.h"

namespace brainscape::fw {
daisy::DaisySeed& Seed();
}  // namespace brainscape::fw
