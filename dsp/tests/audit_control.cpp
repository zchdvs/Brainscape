// Negative control for the symbol audit (docs/design/determinism-profile.md §6.4): an
// engine call left on libm. The audit must reject this library; if it passes, the
// audit has lost its coverage.
#include <cmath>

float BrainscapeAuditControl(float semitones) { return std::exp2(semitones * (1.0f / 12.0f)); }
