#pragma once
#include <cstdint>

// Cycle-exact microbenchmarks of the Cortex-M7 FPU for the denormal decision of
// determinism-profile.md §4.2 (test a) and the flush idiom of §4.3 (open question §8.3 Q5).
// Each runs `iterations` loops of 16 dependent instructions with interrupts off and returns
// the DWT cycles; per-instruction cost is cycles / (16 * iterations), the loop branch adding
// at most about 1/16 cycle per instruction.
namespace brainscape::fw::bench {

enum class FpOp : uint8_t {
  Mul,       // vmul.f32 x = x * a
  Add,       // vadd.f32 x = x + a
  Div,       // vdiv.f32 x = x / a
  Sqrt,      // vsqrt.f32 x = sqrt(x)
  MulPair,   // vmul.f32 x = (x * a) * b, two instructions per step: an underflowing product
             // and its way back, so every other result is subnormal when a = 2^-100
  MulF64,    // vmul.f64 x = x * a
};

// x and a are the operand bit patterns (binary32; MulF64 widens them); b is MulPair's second
// factor. `fpscr` is installed for the run and the caller's word restored after.
uint32_t RunFpChain(FpOp op, uint32_t xBits, uint32_t aBits, uint32_t bBits, uint32_t fpscr,
                    uint32_t iterations);

// The state-update-plus-flush pattern of the engine's recursive filters (detail/FlushTiny.h:
// the bit-pattern test |x| < 1e-20f), eight independent one-poles per step, against the same
// loop without the flush and with the two-compare form the profile first proposed. Returns
// cycles for `iterations` steps.
enum class FlushForm : uint8_t { None, BitTest, Compare };
uint32_t RunFlushLoop(FlushForm form, uint32_t iterations);

}  // namespace brainscape::fw::bench
