#include "Metrics.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

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

// A least-squares line through (t, y): its slope and its value at `at`.
struct Line {
  double slope = 0, at = 0;
};
Line Fit(const std::vector<double>& t, const std::vector<double>& y, size_t from, size_t to, double at) {
  double mt = 0, my = 0;
  for (size_t k = from; k < to; ++k) {
    mt += t[k];
    my += y[k];
  }
  const auto n = static_cast<double>(to - from);
  mt /= n;
  my /= n;
  double sty = 0, stt = 0;
  for (size_t k = from; k < to; ++k) {
    sty += (t[k] - mt) * (y[k] - my);
    stt += (t[k] - mt) * (t[k] - mt);
  }
  Line l;
  l.slope = stt > 0 ? sty / stt : 0.0;
  l.at    = my + l.slope * (at - mt);
  return l;
}

// The frame after the last sample above -70 dBFS, or 0.
size_t TailEnd(const Stereo& s) {
  const double thr = std::pow(10.0, kTailDbfs / 20.0);
  for (size_t i = s.Frames(); i-- > 0;) {
    if (std::fabs(static_cast<double>(s.l[i])) > thr || std::fabs(static_cast<double>(s.r[i])) > thr) return i + 1;
  }
  return 0;
}

// Each sample step against the RMS of the steps around it (Metrics.h, kClickHalfWidth): the
// largest score of either channel. Running sums in binary64 over the squared steps.
void Clicks(const Stereo& s, Metrics* m) {
  const size_t n = s.Frames();
  const size_t h = kClickHalfWidth;
  if (n < 2 * h + 3) return;
  std::vector<double> d(n, 0.0), c(n + 1, 0.0);
  for (int ch = 0; ch < 2; ++ch) {
    const std::vector<float>& x = ch == 0 ? s.l : s.r;
    for (size_t i = 1; i < n; ++i) {
      const double v = static_cast<double>(x[i]) - static_cast<double>(x[i - 1]);
      d[i]           = std::isfinite(v) ? v : 0.0;
    }
    for (size_t i = 0; i < n; ++i) c[i + 1] = c[i] + d[i] * d[i];
    for (size_t i = h + 1; i + h + 1 < n; ++i) {
      const double step = std::fabs(d[i]);
      if (step < kClickStepFloor) continue;
      const double around = (c[i] - c[i - h]) + (c[i + 1 + h] - c[i + 1]);
      const double rms    = std::sqrt(std::max(0.0, around) / static_cast<double>(2 * h));
      const double score  = step / std::max(rms, kClickRmsFloor);
      if (score > m->click) {
        m->click      = score;
        m->clickFrame = i;
        m->clickStep  = step;
      }
    }
  }
}

}  // namespace

void MeasureTail(const Stereo& probe, size_t span, Metrics* m) {
  const size_t frames = probe.Frames();
  span                = std::min(span, frames);
  const size_t silent = frames - span;
  m->tailProbeSeconds = static_cast<double>(silent) / kRate;
  const size_t end    = TailEnd(probe);
  m->tailEstimated    = false;
  m->tailDecayDbPerS  = 0;
  m->tailSeconds      = end > span ? static_cast<double>(end - span) / kRate : 0.0;
  m->tailFinite       = frames - end >= kTailMargin;
  if (m->tailFinite) return;  // it ended inside the probe: the measured time
  // The probe's last kTailFitFrames (at most its silence) in 1 s windows: RMS level in dB.
  const size_t fit = std::min<size_t>(kTailFitFrames, silent - silent % kRate);
  std::vector<double> t, y;
  double              lastPeak = 0, lastMs = 0;
  for (size_t a = frames - fit; a + kRate <= frames; a += kRate) {
    double acc = 0, peak = 0;
    for (size_t i = a; i < a + kRate; ++i) {
      for (const float x : {probe.l[i], probe.r[i]}) {
        if (!std::isfinite(x)) continue;
        acc += 0.5 * static_cast<double>(x) * x;
        peak = std::max(peak, std::fabs(static_cast<double>(x)));
      }
    }
    lastPeak = peak;
    lastMs   = acc / kRate;
    if (acc <= 0) continue;  // exact silence between repeats carries no level
    t.push_back(static_cast<double>(a + kRate / 2 - span) / kRate);
    y.push_back(10.0 * std::log10(acc / kRate));
  }
  if (t.size() < 10) return;  // too little to fit: unending as far as the probe shows
  const double tEnd  = static_cast<double>(silent) / kRate;
  const Line   all   = Fit(t, y, 0, t.size(), tEnd);
  const Line   first = Fit(t, y, 0, t.size() / 2, tEnd);
  const Line   last  = Fit(t, y, t.size() / 2, t.size(), tEnd);
  m->tailDecayDbPerS = -all.slope;
  if (all.slope > -kMinDecayDbPerS || first.slope > -kMinDecayDbPerS / 2 || last.slope > -kMinDecayDbPerS / 2) {
    return;  // flat, rising, too slow, or a fall only one half shows: unending
  }
  // The peak level at the probe's end: the line plus the last second's crest (peak over RMS).
  const double crest = lastPeak > 0 && lastMs > 0 ? Db20(lastPeak) - 10.0 * std::log10(lastMs) : 0.0;
  const double peakEnd = all.at + crest;
  m->tailFinite        = true;
  m->tailEstimated     = true;
  m->tailSeconds       = tEnd + std::max(0.0, peakEnd - kTailDbfs) / -all.slope;
}

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
  Clicks(s, &m);
  const Loudness k(s);
  m.loudness = k.Integrated(0, m.span);
  for (size_t from = 0; from / kSubBlock + kShortTermSubs <= k.SubBlocks(); from += kRate) {
    m.shortTerm.push_back(k.ShortTerm(from));
  }
  m.features = Features(s, 0, m.span);
  return m;
}

std::string TailText(const Metrics& m) {
  char buf[64];
  if (!m.tailFinite) {
    std::snprintf(buf, sizeof buf, "unending (over %.1f s)", m.tailSeconds);
  } else if (m.tailEstimated) {
    std::snprintf(buf, sizeof buf, m.tailSeconds < 100 ? "about %.1f s (extrapolated)" : "about %.0f s (extrapolated)",
                  m.tailSeconds);
  } else if (m.tailProbeSeconds > 0) {
    std::snprintf(buf, sizeof buf, "%.2f s (%.0f s probe)", m.tailSeconds, m.tailProbeSeconds);
  } else {
    std::snprintf(buf, sizeof buf, "%.2f s", m.tailSeconds);
  }
  return buf;
}

}  // namespace bsa
