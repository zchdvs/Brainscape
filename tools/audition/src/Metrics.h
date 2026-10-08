#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Render.h"

// The pre-screen's measurements (docs/design/mode-compiler.md §11.3), desktop-only and outside
// the sound: binary64 arithmetic and libm here change no render, only a printed level, which is
// rounded to 0.1 before anything compares it. Levels are K-weighted loudness, ITU-R BS.1770-4: the
// two-stage K filter at 48 kHz, 400 ms gating blocks with 75 % overlap, the -70 LKFS absolute and
// -10 LU relative gates; the same measurement as the Mix-law probe
// (tools/parity/modes/mixlaw/mixlaw.cpp), which reproduced the record's tables.
namespace bsa {

inline constexpr double   kSilentDb      = -200.0;  // a level below the absolute gate, or silence
inline constexpr double   kTailDbfs      = -70.0;   // the tail's end
inline constexpr uint32_t kSubBlock      = kRate / 10;  // 100 ms: the gating blocks' hop
inline constexpr uint32_t kTailMargin    = kRate / 2;   // a measured tail ends 0.5 s before the render
inline constexpr uint32_t kShortTermSubs = 30;          // the 3 s short-term window, in sub-blocks
// A tail still above -70 dBFS when the render ends (kTailMargin before it) is measured again on a
// probe: the same request with kTailProbeFrames of silence after the input. When the probe's tail
// ends inside it, that is the time; when it does not, the probe's last kTailFitFrames decide: the
// RMS level of each 1 s window there, in dB, is fitted with a line, and a tail falls when the line
// falls by at least kMinDecayDbPerS and so does each half's line by half that, so a fall the fit
// finds in noise does not count. A falling tail is extended to -70 dBFS (its peak level, the RMS
// line plus the last second's crest); a flat, rising or slower one is unending.
inline constexpr uint32_t kTailProbeFrames = 60 * kRate;
inline constexpr uint32_t kTailFitFrames   = 30 * kRate;
inline constexpr double   kMinDecayDbPerS  = 0.2;
// Clicks: a sample step scores against the steps around it, the RMS of the kClickHalfWidth steps
// on each side (the step itself left out, the RMS floored at kClickRmsFloor, -80 dBFS); steps
// under kClickStepFloor (-40 dBFS) are not scored. A smooth waveform scores under 2 whatever its
// pitch; a discontinuity in it scores in the tens.
inline constexpr uint32_t kClickHalfWidth = kRate / 1000;  // 1 ms
inline constexpr double   kClickStepFloor = 0.01;
inline constexpr double   kClickRmsFloor  = 1e-4;

// K-weighted power per 100 ms sub-block (the sum over both channels of the mean square), the K
// filter run from frame 0: every level below derives from it.
class Loudness {
 public:
  explicit Loudness(const Stereo& s);
  // Integrated loudness in LUFS of the gating blocks inside [from, to) (frames, rounded to
  // sub-blocks); kSilentDb when no block passes the absolute gate.
  double Integrated(size_t from, size_t to) const;
  // The 3 s short-term loudness of the window starting at `from` (no gate); kSilentDb for
  // silence.
  double ShortTerm(size_t from) const;
  size_t SubBlocks() const { return sub_.size(); }

 private:
  std::vector<double> sub_;
};

// Features of a span of frames, for the Response check's proxies: the brightness, the frequency
// of the sinusoid whose first differences carry the same share of its energy (2 asin(sqrt(D/E)/2)
// rad per sample, D and E the energies of the differences and of the signal), and the envelope's
// variation, the standard deviation in dB of the 10 ms RMS envelope over its frames above -60
// dBFS.
struct SpanFeatures {
  double brightnessHz   = 0;
  double envelopeVarDb  = 0;
};
SpanFeatures Features(const Stereo& s, size_t from, size_t to);

struct Metrics {
  size_t   frames       = 0;
  size_t   span         = 0;          // the frames the input sounds in: the levels' span
  double   peakDbfs     = kSilentDb;  // the largest |sample| of either channel
  uint64_t overFull     = 0;          // samples with |x| > 1: clipped by the pedal's codec
  uint64_t nonFinite    = 0;          // NaN or infinite samples
  uint64_t subnormal    = 0;          // nonzero samples below FLT_MIN
  double   loudness     = kSilentDb;  // integrated, over [0, span)
  // From the span's end to the last sample above -70 dBFS. A tail the render does not see end is
  // not finite until a probe (MeasureTail) measures or extrapolates it; for an unending tail, the
  // time the render (or the probe) shows.
  double   tailSeconds  = 0;
  bool     tailFinite   = true;       // ended inside the render or its probe, or extrapolated
  bool     tailEstimated = false;     // extrapolated from the probe's fitted fall
  double   tailProbeSeconds = 0;      // the probe's silence, when one measured the tail
  double   tailDecayDbPerS = 0;       // the probe's fitted fall, when it was fitted
  double   maxStep      = 0;          // the largest |x[n] - x[n-1]| of either channel
  size_t   maxStepFrame = 0;
  double   click        = 0;          // the largest step score (kClickHalfWidth), 0 for none
  size_t   clickFrame   = 0;
  double   clickStep    = 0;          // that step
  SpanFeatures features;              // over [0, span)
  std::vector<double> shortTerm;      // 3 s windows every 1 s from frame 0, while they fit
};

Metrics Measure(const Stereo& s, size_t span);
// The tail of `m`'s render from `probe`, the same request with a longer silence (kTailProbeFrames),
// whose input sounds in the same `span`.
void MeasureTail(const Stereo& probe, size_t span, Metrics* m);

// A tail's text: "2.94 s", "41.20 s (60 s probe)", "about 146 s (extrapolated)" or
// "unending (over 60.0 s)".
std::string TailText(const Metrics& m);

double Db20(double amplitude);  // 20 log10, kSilentDb for 0
double Round1(double v);        // to 0.1, what the pre-screen compares of levels

}  // namespace bsa
