// A compiler-library TU in namespace bsc that reads the dsp descriptor table, as a schema
// table or a range check would (design §8.1: "namespace bsc keeps the symbol-scan rule true").
#include "brainscape/Params.h"
namespace bsc {
const char* LeafName(unsigned i) { return brainscape::kParamTable[i].name; }
float LeafMax(unsigned i) { return brainscape::kParamTable[i].max; }
}
