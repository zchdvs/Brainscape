# Applies the determinism-profile edits to the scratch copy dsp_det/ (never the repo).
import sys, os
os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'dsp_det'))

def sub(path, old, new, count=1):
    s = open(path, encoding='utf-8').read()
    n = s.count(old)
    if n != count:
        raise SystemExit(f"{path}: expected {count} of {old!r}, found {n}")
    s = s.replace(old, new)
    open(path, 'w', encoding='utf-8', newline='\n').write(s)

def suball(path, old, new):
    s = open(path, encoding='utf-8').read()
    n = s.count(old)
    s = s.replace(old, new)
    open(path, 'w', encoding='utf-8', newline='\n').write(s)
    return n

# --- DetMath: sqrt wrappers
p = 'include/brainscape/DetMath.h'
sub(p, '#include <cstdint>\n#include <cstring>\n',
    '#include <cstdint>\n#include <cstring>\n#if !defined(__GNUC__)\n'
    '#include <cmath>  // MSVC: std::sqrt lowers to sqrtss/sqrtsd (IEEE correctly rounded)\n#endif\n')
sub(p, '// ── exp family', '''// IEEE-754 requires sqrt to be correctly rounded, so it is the one non-basic
// operation the profile allows. GCC/Clang: build with -fno-math-errno or the
// builtin keeps an errno fallback call to sqrtf/sqrt for negative inputs.
#if defined(__GNUC__)
inline float  SqrtF(float x) noexcept { return __builtin_sqrtf(x); }
inline double SqrtD(double x) noexcept { return __builtin_sqrt(x); }
#else
inline float  SqrtF(float x) noexcept { return std::sqrt(x); }
inline double SqrtD(double x) noexcept { return std::sqrt(x); }
#endif

// ── exp family''')

# --- GrainMath.h
p = 'include/brainscape/GrainMath.h'
sub(p, '#include <cmath>\n#include <cstdint>\n', '#include <cstdint>\n\n#include "brainscape/DetMath.h"\n')
sub(p, '  return std::exp2(st * (1.0f / 12.0f));',
    '  return detmath::Exp2F(st * (1.0f / 12.0f));  // determinism profile: in-tree exp2')

# --- Smoother.h
p = 'include/brainscape/detail/Smoother.h'
sub(p, '#include <cmath>\n', '#include "brainscape/DetMath.h"\n')
sub(p, 'coef = -static_cast<float>(std::expm1(-1.0 / (tauMs * 0.001 * sr)));',
    'coef = -static_cast<float>(detmath::Expm1D(-1.0 / (tauMs * 0.001 * sr)));')

# --- Engine.cpp
p = 'src/Engine.cpp'
sub(p, '#include <cassert>\n#include <cmath>\n#include <cstring>\n', '#include <cassert>\n#include <cstring>\n')
sub(p, '#include "brainscape/DenormalGuard.h"\n', '#include "brainscape/DenormalGuard.h"\n#include "brainscape/DetMath.h"\n')
sub(p, '    windowLut_[i]  = static_cast<float>(0.5 * (1.0 - std::cos(3.14159265358979323846 * x)));',
    '    const double c  = detmath::CosPi(x);  // cos(pi*x), exact half-turn reduction\n'
    '    const double om = 1.0 - c;\n'
    '    windowLut_[i]   = static_cast<float>(0.5 * om);')
sub(p, 'outGain_.target = std::exp2(value * 0.16609640474436813f);',
    'outGain_.target = detmath::Exp2F(value * 0.16609640474436813f);')
sub(p, 'static_cast<uint32_t>(std::lround(get(ParamId::GrainSizeMs) * 0.001 * sr));',
    'static_cast<uint32_t>(detmath::RoundHalfAwayI32(get(ParamId::GrainSizeMs) * 0.001 * sr));')
sub(p, 'std::fabs(', 'detmath::Abs(', 2)
sub(p, 'norm_.target  = std::pow(gp_.targetVoices, -p);', 'norm_.target  = detmath::PowF(gp_.targetVoices, -p);')
sub(p, 'bool Engine::Init(const EngineConfig& cfg, const Arenas& arenas) noexcept {\n  ready_ = false;',
    'bool Engine::Init(const EngineConfig& cfg, const Arenas& arenas) noexcept {\n'
    '  ScopedDenormalGuard guard;  // profile: tables/coefs built under the pinned FP env\n  ready_ = false;')
sub(p, 'void Engine::Reset() noexcept {\n  if (!ready_) return;',
    'void Engine::Reset() noexcept {\n  if (!ready_) return;\n'
    '  ScopedDenormalGuard guard;  // profile: rebuilds run under the pinned FP env')

# --- Granular.cpp
p = 'src/Granular.cpp'
sub(p, '#include <cmath>\n', '#include "brainscape/DetMath.h"\n')
sub(p, 'static_cast<uint32_t>(std::lround(d))', 'static_cast<uint32_t>(detmath::RoundHalfAwayI32(d))')
sub(p, 'static_cast<int64_t>(std::llround(static_cast<double>(ratio) * kFix))',
    'detmath::RoundHalfAwayI64(static_cast<double>(ratio) * kFix)')
sub(p, '''  g.gainL = std::cos(pan * 1.5707963267948966f);
  g.gainR = std::sin(pan * 1.5707963267948966f);''', '''  {
    double s, c;
    detmath::SinCosD(static_cast<double>(pan * 1.5707963267948966f), &s, &c);
    g.gainL = static_cast<float>(c);
    g.gainR = static_cast<float>(s);
  }''')
sub(p, 'const float exp = -std::log(1.0f - u * 0.999f) * spacing;',
    'const float exp = -detmath::LogF(1.0f - u * 0.999f) * spacing;')

# --- OnsetDetector.cpp
p = 'src/OnsetDetector.cpp'
sub(p, '#include <cmath>\n', '#include "brainscape/DetMath.h"\n')
sub(p, 'static_cast<int64_t>(std::ceil(0.020 * sampleRate / kOnsetHop));',
    'static_cast<int64_t>(detmath::CeilSmall(0.020 * sampleRate / kOnsetHop));')
sub(p, 'whitenDecay_ = static_cast<float>(std::exp(-(static_cast<double>(kOnsetHop) / sampleRate) / 0.4));',
    'whitenDecay_ = static_cast<float>(detmath::ExpD(-(static_cast<double>(kOnsetHop) / sampleRate) / 0.4));')
sub(p, '''    hann_[i] = static_cast<float>(
        0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(i) / kOnsetFftSize)));''',
    '''    // cos(2*pi*i/N) == cos(pi * (2i/N)): the half-turn argument is exact (dyadic).
    const double c  = detmath::CosPi(static_cast<double>(2u * i) / kOnsetFftSize);
    const double om = 1.0 - c;
    hann_[i] = static_cast<float>(0.5 * om);''')
sub(p, '''    twCos_[k] = static_cast<float>(std::cos(2.0 * kPi * k / kOnsetFftSize));
    twSin_[k] = static_cast<float>(-std::sin(2.0 * kPi * k / kOnsetFftSize));''',
    '''    double s, c;
    detmath::SinCosPi(static_cast<double>(2u * k) / kOnsetFftSize, &s, &c);
    twCos_[k] = static_cast<float>(c);
    twSin_[k] = static_cast<float>(-s);''')
sub(p, 'const float mag = std::sqrt(re_[k] * re_[k] + im_[k] * im_[k]);',
    'const float mag = detmath::SqrtF(re_[k] * re_[k] + im_[k] * im_[k]);')
sub(p, 'namespace {\nconstexpr double kPi = 3.14159265358979323846;\n}\n\n', '')

# --- PostChain.cpp
p = 'src/PostChain.cpp'
sub(p, '#include <cmath>\n', '#include "brainscape/DetMath.h"\n')
sub(p, 'return -static_cast<float>(std::expm1(-6.283185307179586 * cutoffHz / sr));',
    'return -static_cast<float>(detmath::Expm1D(-6.283185307179586 * cutoffHz / sr));')
n = suball(p, 'static_cast<uint32_t>(std::lround(', 'static_cast<uint32_t>(detmath::RoundHalfAwayI32(')
print('PostChain lround replaced', n)
sub(p, 'svfFreq_ = static_cast<float>(2.0 * std::sin(3.14159265358979 * (f < 0.25 ? f : 0.25)));',
    'svfFreq_ = static_cast<float>(2.0 * detmath::SinD(3.14159265358979 * (f < 0.25 ? f : 0.25)));')
sub(p, 'float damp      = 2.0f * (1.0f - std::pow(r, 0.25f));',
    '''// r^0.25 = sqrt(sqrt(r)) in double (both correctly rounded), one final rounding.
  const float r4  = static_cast<float>(detmath::SqrtD(detmath::SqrtD(static_cast<double>(r))));
  float damp      = 2.0f * (1.0f - r4);''')
sub(p, '''  svfWa_          = std::cos(fr * 1.5707963267948966f);
  svfWb_          = std::sin(fr * 1.5707963267948966f);''', '''  {
    double s, c;
    detmath::SinCosD(static_cast<double>(fr * 1.5707963267948966f), &s, &c);
    svfWa_ = static_cast<float>(c);
    svfWb_ = static_cast<float>(s);
  }''')
n = suball(p, 'std::sqrt(', 'detmath::SqrtF(')
print('PostChain sqrt replaced', n)

# --- DenormalGuard: pin the rounding mode too
p = 'include/brainscape/DenormalGuard.h'
sub(p, 'ScopedDenormalGuard() noexcept : saved_(_mm_getcsr()) { _mm_setcsr(saved_ | 0x8040u); }',
    '// Profile: also force RC = round-to-nearest (bits 13-14) - a host that left a\n'
    '  // directed rounding mode set would otherwise change every result.\n'
    '  ScopedDenormalGuard() noexcept : saved_(_mm_getcsr()) { _mm_setcsr((saved_ & ~0x6000u) | 0x8040u); }')
sub(p, '    const uint32_t v = saved_ | (1u << 24);', '    const uint32_t v = (saved_ & ~(3u << 22)) | (1u << 24);  // RMode = RN, FZ = 1')
sub(p, '    const uint64_t v = saved_ | (1ull << 24);', '    const uint64_t v = (saved_ & ~(3ull << 22)) | (1ull << 24);  // RMode = RN, FZ = 1')
print('ok')
