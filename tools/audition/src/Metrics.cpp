#include "Metrics.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace bsa {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Biquad {
  double b0, b1, b2, a1, a2;
  double z1 = 0, z2 = 0;
  double Run(double x) {
    const double y = b0 * x + z1;
    z1             = b1 * x - a1 * y + z2;
    z2             = b2 * x - a2 * y;
    return y;
  }
};

double PowerDb(double meanSquare) { return meanSquare > 0 ? 10.0 * std::log10(meanSquare) : kSilentDb; }
double Lufs(double meanSquare) {
  return meanSquare > 0 ? -0.691 + 10.0 * std::log10(meanSquare) : kSilentDb;
}

}  // namespace

double Db20(double amplitude) { return amplitude > 0 ? 20.0 * std::log10(amplitude) : kSilentDb; }

double Round1(double v) { return std::floor(v * 10.0 + 0.5) / 10.0; }

Loudness::Loudness(const Stereo& s) {
  const size_t frames = s.Frames();
  sub_.assign(frames / kSubBlock, 0.0);
  for (int c = 0; c < 2; ++c) {
    // BS.1770-4's K filter at 48 kHz: the high-shelf pre-filter, then the RLB high-pass.
    Biquad shelf{1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241,
                 0.73248077421585};
    Biquad hp{1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621};
    const std::vector<float>& x = c == 0 ? s.l : s.r;
    for (size_t k = 0; k < sub_.size(); ++k) {
      double acc = 0;
      for (size_t i = k * kSubBlock; i < (k + 1) * kSubBlock; ++i) {
        const double y = hp.Run(shelf.Run(x[i]));
        acc += y * y;
      }
      sub_[k] += acc / kSubBlock;
    }
  }
}

double Loudness::Integrated(size_t from, size_t to) const {
  const size_t a = (from + kSubBlock - 1) / kSubBlock;
  const size_t b = std::min(to / kSubBlock, sub_.size());
  std::vector<double> z;  // 400 ms blocks: four sub-blocks, a hop of one
  for (size_t k = a; k + 4 <= b; ++k) z.push_back((sub_[k] + sub_[k + 1] + sub_[k + 2] + sub_[k + 3]) / 4.0);
  auto gated = [&](double gate) {
    double acc = 0;
    size_t n   = 0;
    for (const double ms : z) {
      if (Lufs(ms) > -70.0 && Lufs(ms) > gate) {
        acc += ms;
        ++n;
      }
    }
    return n == 0 ? 0.0 : acc / static_cast<double>(n);
  };
  const double absolute = gated(-70.0);
  if (absolute <= 0) return kSilentDb;
  return Lufs(gated(Lufs(absolute) - 10.0));
}

double Loudness::ShortTerm(size_t from) const {
  const size_t a = from / kSubBlock;
  if (a + kShortTermSubs > sub_.size()) return kSilentDb;
  double acc = 0;
  for (size_t k = a; k < a + kShortTermSubs; ++k) acc += sub_[k];
  return Lufs(acc / kShortTermSubs);
}

SpanFeatures Features(const Stereo& s, size_t from, size_t to) {
  SpanFeatures f;
  to = std::min(to, s.Frames());
  if (to <= from + 1) return f;
  double e = 0, d = 0;
  for (size_t i = from + 1; i < to; ++i) {
    for (int c = 0; c < 2; ++c) {
      const std::vector<float>& x = c == 0 ? s.l : s.r;
      const double              v = x[i], dv = static_cast<double>(x[i]) - x[i - 1];
      e += v * v;
      d += dv * dv;
    }
  }
  if (e > 0) {
    const double ratio = std::min(4.0, d / e);
    f.brightnessHz     = 2.0 * std::asin(std::sqrt(ratio) / 2.0) * kRate / (2.0 * kPi);
  }
  const size_t        frame = kRate / 100;  // 10 ms
  std::vector<double> env;
  for (size_t a = from; a + frame <= to; a += frame) {
    double acc = 0;
    for (size_t i = a; i < a + frame; ++i) {
      acc += 0.5 * (static_cast<double>(s.l[i]) * s.l[i] + static_cast<double>(s.r[i]) * s.r[i]);
    }
    const double db = PowerDb(acc / frame);
    if (db > -60.0) env.push_back(db);
  }
  if (env.size() > 1) {
    double mean = 0;
    for (const double v : env) mean += v;
    mean /= static_cast<double>(env.size());
    double var = 0;
    for (const double v : env) var += (v - mean) * (v - mean);
    f.envelopeVarDb = std::sqrt(var / static_cast<double>(env.size()));
  }
  return f;
}

Metrics Measure(const Stereo& s, size_t span) {
  Metrics m;
  m.frames         = s.Frames();
  m.span           = std::min(span, m.frames);
  const double thr = std::pow(10.0, kTailDbfs / 20.0);
  double       peak = 0;
  int64_t      last = -1;  // the last frame above -70 dBFS
  float        prev[2] = {0.f, 0.f};
  for (size_t i = 0; i < m.frames; ++i) {
    const float v[2] = {s.l[i], s.r[i]};
    bool        loud = false;
    for (int c = 0; c < 2; ++c) {
      const float x = v[c];
      if (!std::isfinite(x)) {
        ++m.nonFinite;
        continue;
      }
      const double a = std::fabs(static_cast<double>(x));
      if (x != 0.f && a < FLT_MIN) ++m.subnormal;
      if (a > 1.0) ++m.overFull;
      peak = std::max(peak, a);
      loud = loud || a > thr;
      if (i > 0 && std::isfinite(prev[c])) {
        const double step = std::fabs(static_cast<double>(x) - prev[c]);
        if (step > m.maxStep) {
          m.maxStep      = step;
          m.maxStepFrame = i;
        }
      }
    }
    prev[0] = v[0];
    prev[1] = v[1];
    if (loud) last = static_cast<int64_t>(i);
  }
  m.peakDbfs = Db20(peak);
  const auto end = static_cast<int64_t>(m.span);
  m.tailSeconds  = last + 1 > end ? static_cast<double>(last + 1 - end) / kRate : 0.0;
  m.tailFinite   = static_cast<int64_t>(m.frames) - (last + 1) >= static_cast<int64_t>(kTailMargin);
  const Loudness k(s);
  m.loudness = k.Integrated(0, m.span);
  for (size_t from = 0; from / kSubBlock + kShortTermSubs <= k.SubBlocks(); from += kRate) {
    m.shortTerm.push_back(k.ShortTerm(from));
  }
  m.features = Features(s, 0, m.span);
  return m;
}

}  // namespace bsa
