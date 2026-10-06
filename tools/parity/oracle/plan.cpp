#include <initializer_list>
#include <cstdio>
#include "brainscape/Engine.h"
using namespace brainscape;
int main() {
  for (unsigned h : {22u, 21u, 20u}) {
    EngineConfig c; c.historyFrames = 1u << h; c.maxBlockSize = 512;
    MemoryPlan p = PlanMemory(c);
    std::printf("hist=2^%u  Hot=%zu B  Warm=%zu B (%.1f KiB)  Bulk=%zu B (%.2f MiB)  sizeof(Engine)=%zu B\n", h,
      p.bytes[0], p.bytes[1], p.bytes[1]/1024.0, p.bytes[2], p.bytes[2]/1048576.0, sizeof(Engine));
  }
}
