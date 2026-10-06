# Option B prototype: explicit, source-placed FMAs in two hot loops (Hermite read +
# SVF tick) on top of the determinism profile. fmaf is exactly specified by IEEE-754
# (one rounding), so explicit fusion is as portable as +,-,*,/ — the compiler only must
# not ADD fusions elsewhere (contraction stays off).
import os
os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'dsp_fma'))

def sub(path, old, new, count=1):
    s = open(path, encoding='utf-8').read()
    n = s.count(old)
    if n != count:
        raise SystemExit(f"{path}: expected {count} of {old!r}, found {n}")
    s = s.replace(old, new)
    open(path, 'w', encoding='utf-8', newline='\n').write(s)

p = 'include/brainscape/DetMath.h'
sub(p, '// ── exp family', '''// Explicit fused multiply-add: IEEE-754 fusedMultiplyAdd (single rounding) — exactly
// specified, so placing it explicitly is portable. GCC/Clang: inline vfma/vfmadd when
// the ISA has it (Cortex-M7 FPv5, x86-64-v3), else a call to libm fmaf (exact).
// MSVC: std::fma -> UCRT fmaf (FMA3 at runtime, exact software fallback otherwise).
#if defined(__GNUC__)
inline float FmaF(float a, float b, float c) noexcept { return __builtin_fmaf(a, b, c); }
#else
inline float FmaF(float a, float b, float c) noexcept { return std::fma(a, b, c); }
#endif

// ── exp family''')

p = 'src/Granular.cpp'
sub(p, '  return ((((a * fr) - bNeg) * fr + c) * fr + x0) * kInvScale;',
    '  float h = detmath::FmaF(a, fr, -bNeg);\n'
    '  h = detmath::FmaF(h, fr, c);\n'
    '  h = detmath::FmaF(h, fr, x0);\n'
    '  return h * kInvScale;')
sub(p, '          wetL[n] += ReadHermite(ring_, mask_, pos, 0) * env * gl;\n'
       '          wetR[n] += ReadHermite(ring_, mask_, pos, 1) * env * gr;',
    '          wetL[n] = detmath::FmaF(ReadHermite(ring_, mask_, pos, 0) * env, gl, wetL[n]);\n'
    '          wetR[n] = detmath::FmaF(ReadHermite(ring_, mask_, pos, 1) * env, gr, wetR[n]);')

p = 'src/PostChain.cpp'
sub(p, '''              const float notch = in - dp * s[1];
              s[0] += fq * s[1];
              const float high = notch - s[0];
              s[1] += fq * high;
              outs[0] += 0.5f * s[0];
              outs[1] += 0.5f * s[1];
              outs[2] += 0.5f * high;
              outs[3] += 0.5f * notch;''', '''              const float notch = detmath::FmaF(-dp, s[1], in);
              s[0] = detmath::FmaF(fq, s[1], s[0]);
              const float high = notch - s[0];
              s[1] = detmath::FmaF(fq, high, s[1]);
              outs[0] = detmath::FmaF(0.5f, s[0], outs[0]);
              outs[1] = detmath::FmaF(0.5f, s[1], outs[1]);
              outs[2] = detmath::FmaF(0.5f, high, outs[2]);
              outs[3] = detmath::FmaF(0.5f, notch, outs[3]);''')
sub(p, '            return outs[seg] * wa + outs[seg + 1] * wb;',
    '            return detmath::FmaF(outs[seg], wa, outs[seg + 1] * wb);')
print('ok')
