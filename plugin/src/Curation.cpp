#include "Curation.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

#include "Compile.h"  // brainscape_compiler
#include "FactoryModes.h"
#include "Lint.h"
#include "PluginProcessor.h"
#include "Text.h"
#include "brainscape/ModeEval.h"
#include "brainscape/ParamDisplay.h"
#include "brainscape/SoundRevision.h"

namespace brainscape::plugin {

namespace {

// Re-measuring after an edit waits for the knobs to rest this long; so does the live compile.
constexpr uint32_t kMatchSettleMs = 400;
constexpr uint32_t kLintSettleMs  = 150;

uint32_t Bits(float v) {
  uint32_t u = 0;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

uint32_t NowMs() { return juce::Time::getMillisecondCounter(); }

ParamId MacroId(size_t k) { return static_cast<ParamId>(static_cast<uint32_t>(ParamId::MacroActivity) + k); }

bool IsFactory(const bsc::Document& d) { return d.id.rfind("factory.", 0) == 0; }

// A factory package's .bsp path within firmware/factory/ ("lull.bsp", "reserve/runaway.bsp").
juce::String FactoryBsp(size_t index) {
  return juce::String(Factory(index).path).upToLastOccurrenceOf(".json", false, false) + ".bsp";
}

bool ReadBytes(const juce::File& file, std::vector<uint8_t>* out, juce::String* error) {
  // A document over the compiler's bound is refused unread (E12); a package is far smaller.
  if (!file.existsAsFile()) {
    *error = "no such file: " + file.getFullPathName();
    return false;
  }
  if (file.getSize() > static_cast<juce::int64>(bsc::kMaxDocumentBytes)) {
    *error = file.getFileName() + " is over 1 MiB: no preset document is that large";
    return false;
  }
  juce::MemoryBlock mb;
  if (!file.loadFileAsData(mb)) {
    *error = "cannot read " + file.getFullPathName();
    return false;
  }
  const auto* p = static_cast<const uint8_t*>(mb.getData());
  out->assign(p, p + mb.getSize());
  return true;
}

// The bytes into a temporary file beside `file` (`temp`, made for it).
bool WriteTemp(const juce::TemporaryFile& temp, const juce::File& file, const void* data, size_t size,
               juce::String* error) {
  juce::FileOutputStream out(temp.getFile());
  if (!out.openedOk() || !out.write(data, size)) {
    *error = "cannot write " + file.getFullPathName();
    return false;
  }
  out.flush();
  if (out.getStatus().failed()) {
    *error = "cannot write " + file.getFullPathName() + ": " + out.getStatus().getErrorMessage();
    return false;
  }
  return true;
}

// A directory where a file goes is refused before anything is written: JUCE's replace on POSIX
// falls back to copying into it and reports success, where Windows' ReplaceFile fails.
bool InTheWay(const juce::File& file, juce::String* error) {
  if (!file.isDirectory()) return false;
  *error = "cannot replace " + file.getFullPathName() + ": a directory is in the way";
  return true;
}

// Through a temporary file beside the target, so a failed write leaves the old file whole.
bool WriteBytes(const juce::File& file, const void* data, size_t size, juce::String* error) {
  if (InTheWay(file, error)) return false;
  juce::TemporaryFile temp(file);
  if (!WriteTemp(temp, file, data, size, error)) return false;
  if (!temp.overwriteTargetFileWithTemporary()) {
    *error = "cannot replace " + file.getFullPathName();
    return false;
  }
  return true;
}

// The document and its package, as a pair: both written to temporary files first, then the JSON
// replaced and the package after it; when the package cannot be replaced, the JSON's old bytes
// go back (or the new file goes, when there was none), so the pair on disk is the old one or the
// new one, never a mix. False with *error when nothing changed on disk; *mixed is set when the
// JSON could not be put back either.
bool WritePair(const juce::File& json, const std::string& text, const juce::File& bsp,
               const std::vector<uint8_t>& package, juce::String* error, bool* mixed) {
  *mixed = false;
  if (InTheWay(json, error) || InTheWay(bsp, error)) return false;
  juce::TemporaryFile tj(json), tp(bsp);
  if (!WriteTemp(tj, json, text.data(), text.size(), error) ||
      !WriteTemp(tp, bsp, package.data(), package.size(), error)) {
    return false;
  }
  juce::MemoryBlock old;
  const bool        had = json.existsAsFile();
  if (had && !json.loadFileAsData(old)) {
    *error = "cannot read " + json.getFullPathName() + " to keep it while the pair is replaced";
    return false;
  }
  if (!tj.overwriteTargetFileWithTemporary()) {
    *error = "cannot replace " + json.getFullPathName();
    return false;
  }
  if (tp.overwriteTargetFileWithTemporary()) return true;
  *error = "cannot replace " + bsp.getFullPathName() + "; " + json.getFileName() + " is left as it was";
  juce::String ignored;
  const bool   restored = had ? WriteBytes(json, old.getData(), old.getSize(), &ignored) : json.deleteFile();
  if (!restored) {
    *mixed = true;
    *error = "cannot replace " + bsp.getFullPathName() + ", and " + json.getFileName() +
             " (already replaced) could not be put back: the pair on disk differs";
  }
  return false;
}

juce::String FirstError(const std::vector<bsc::Finding>& findings) {
  for (const bsc::Finding& f : findings) {
    if (f.error) return juce::String(f.code + " " + f.at.pointer + ": " + f.message);
  }
  return findings.empty() ? juce::String() : juce::String(findings.front().message);
}

std::vector<bsc::Finding> ErrorsFirst(std::vector<bsc::Finding> f) {
  std::stable_sort(f.begin(), f.end(), [](const bsc::Finding& a, const bsc::Finding& b) { return a.error && !b.error; });
  return f;
}

// The position CTRL stores for a macro, or null.
float* Position(PresetState& s, ParamId macro) {
  for (uint32_t k = 0; k < s.control.macroCount && k < kMaxMacros; ++k) {
    if (s.control.positions[k].macroId == static_cast<uint32_t>(macro)) return &s.control.positions[k].position;
  }
  return nullptr;
}

}  // namespace

struct CurationSession::Worker {
  std::thread       thread;
  std::atomic<bool> done{false};
};

CurationSession::CurationSession(BrainscapeProcessor& processor) : processor_(processor) {}

// A render runs for seconds (S0-S11 for half a minute): the processor going away must not wait
// for it, so the workers' renders stop at their next block.
CurationSession::~CurationSession() {
  cancel_.store(true, std::memory_order_relaxed);
  JoinWorkers();
}

void CurationSession::JoinWorkers() {
  for (auto& w : workers_) {
    if (w->thread.joinable()) w->thread.join();
  }
  workers_.clear();
}

void CurationSession::StopWorkers(const juce::String& why) {
  if (workers_.empty()) return;
  cancel_.store(true, std::memory_order_relaxed);
  JoinWorkers();
  cancel_.store(false, std::memory_order_relaxed);
  const std::lock_guard<std::mutex> lock(mutex_);
  if (render_.state == RenderStatus::State::Failed && render_.message.startsWith("Stopped")) {
    render_.message = "Stopped: " + why;
  }
}

juce::File CurationSession::DefaultRenderDir() {
  return juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("Brainscape audition");
}

// ── Opening ───────────────────────────────────────────────────────────────────────────────

bool CurationSession::Open(const juce::File& file, juce::String* error) {
  juce::String         local;
  juce::String&        err = error != nullptr ? *error : local;
  std::vector<uint8_t> bytes;
  if (!ReadBytes(file, &bytes, &err)) {
    refused_.clear();
    refusedFile_ = file.getFileName();
    return false;
  }
  if (file.hasFileExtension("bsp")) return OpenPackage(std::move(bytes), {file, -1}, &err);
  return LoadText(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), {file, -1}, false, nullptr,
                  &err);
}

bool CurationSession::OpenFactory(size_t index, juce::String* error) {
  juce::String  local;
  juce::String& err = error != nullptr ? *error : local;
  if (index >= FactoryCount()) {
    err = "no factory mode " + juce::String(static_cast<int>(index));
    return false;
  }
  const FactoryPackage& f = Factory(index);
  return OpenPackage(std::vector<uint8_t>(f.bytes, f.bytes + f.size), {juce::File(), static_cast<int>(index)}, &err);
}

bool CurationSession::OpenPackage(std::vector<uint8_t> bytes, const Origin& origin, juce::String* error) {
  const bsc::DecompileResult d = bsc::Decompile(bytes.data(), bytes.size());
  if (!d.ok) {
    refused_     = ErrorsFirst(d.findings);
    refusedFile_ = Label(origin);
    *error       = Label(origin) + ": not a package this build reads (" + FirstError(d.findings) + ")";
    return false;
  }
  return LoadText(d.json, origin, true, &bytes, error);
}

bool CurationSession::Revert(juce::String* error) {
  if (!open_) return false;
  if (factory_ >= 0) return OpenFactory(static_cast<size_t>(factory_), error);
  return Open(source_, error);
}

juce::String CurationSession::Label(const Origin& origin) const {
  return origin.factory >= 0 ? "factory/" + FactoryBsp(static_cast<size_t>(origin.factory))
                             : origin.file.getFileName();
}

juce::String CurationSession::SourceLabel() const {
  if (!open_) return {};
  if (factory_ >= 0) return "firmware/factory/" + FactoryBsp(static_cast<size_t>(factory_)) + " (built in)";
  return jsonFile_.getFileName();
}

PresetSource CurationSession::Source() const {
  if (!open_) return {};
  return {factory_, juce::String::fromUTF8(stored_->name.c_str()), stored_->family};
}

void CurationSession::Close() {
  CloseDocument();
  adoptSerial_ = processor_.LoadSerial();  // closed by hand: not reopened for what plays now
}

void CurationSession::CloseDocument() {
  StopWorkers("the document closed");
  if (side_ == Side::Stored) SetSide(Side::Working);
  open_    = false;
  factory_ = -1;
  stored_.reset();
  working_.reset();
  storedState_.reset();
  workingSnapshot_.reset();
  detached_.clear();
  pending_.clear();
  pendingPerformance_.clear();
  findings_.clear();
  seenBits_.clear();
  dirty_ = false;
  ResetMatch();
  ApplyMonitorTrim();
}

void CurationSession::ResetMatch() {
  const std::lock_guard<std::mutex> lock(mutex_);
  match_ = LevelMatch{};
  ++matchGen_;
}

bool CurationSession::LoadText(const std::string& text, const Origin& origin, bool fromPackage,
                               const std::vector<uint8_t>* package, juce::String* error, bool play) {
  const juce::String        label = Label(origin);
  auto                      doc   = std::make_unique<bsc::Document>();
  std::vector<bsc::Finding> found;
  if (!bsc::ReadDocumentText(text, {}, doc.get(), &found)) {
    refused_     = ErrorsFirst(found);
    refusedFile_ = label;
    *error       = label + " does not read: " + FirstError(found);
    return false;
  }
  const bsc::CompileResult r = bsc::CompileDocument(*doc);
  if (!r.ok) {
    refused_     = ErrorsFirst(r.findings);
    refusedFile_ = label;
    *error       = label + " does not compile: " + FirstError(r.findings);
    return false;
  }
  // What plays: the package's own bytes when it came from one (what the pedal plays), else the
  // document compiled now.
  const std::vector<uint8_t>& bytes = package != nullptr ? *package : r.package;
  bsc::DecodedPackage         p     = bsc::DecodePackage(bytes.data(), bytes.size());
  if (!p.ok) {
    refused_.clear();
    refusedFile_ = label;
    *error       = label + ": " + juce::String(bsc::DescribeDiagnostic(p.diagnostic));
    return false;
  }
  LoadReport report;
  report.exact = true;
  if (play) {
    if (!processor_.LoadPresetState(*p.state, &report,
                                    {origin.factory, juce::String::fromUTF8(doc->name.c_str()), doc->family})) {
      refused_.clear();
      refusedFile_ = label;
      *error       = label + ": its mode does not validate";
      return false;
    }
  } else {
    const ModeState now = processor_.CurrentMode();
    if (std::memcmp(&now.mode, &p.state->mode, sizeof(ModeBlob)) != 0) {
      *error = label + ": the plugin plays another mode";
      return false;
    }
  }
  StopWorkers("another document opened");  // a render of the document this one replaces
  refused_.clear();
  refusedFile_.clear();
  open_          = true;
  factory_       = origin.factory;
  source_        = origin.file;
  jsonFile_      = origin.factory >= 0 ? juce::File() : origin.file.withFileExtension(".json");
  writesPackage_ = fromPackage || jsonFile_.withFileExtension(".bsp").existsAsFile();
  // What Save would write for the document unedited and derived is the stamped canonical text
  // (r.json, when nothing is pending): the file is that, or Save changes it. A factory
  // package's is its JSON section.
  if (fromPackage && origin.factory < 0) {
    juce::MemoryBlock beside;
    canonical_ = jsonFile_.existsAsFile() && jsonFile_.loadFileAsData(beside) && beside.getSize() == r.json.size() &&
                 std::memcmp(beside.getData(), r.json.data(), r.json.size()) == 0;
  } else {
    canonical_ = text == r.json;
  }
  stampCurrent_  = doc->stamped && doc->soundRev == kSoundRevision &&
                  std::memcmp(doc->soundHash.bytes, r.soundHash.bytes, sizeof r.soundHash.bytes) == 0;
  detached_      = doc->detached;
  stored_        = std::move(doc);
  storedState_   = std::move(p.state);
  side_          = Side::Working;
  workingSnapshot_.reset();
  seenBits_.clear();
  ResetMatch();
  matchStale_ = matchLevel_;
  message_    = play ? "Opened " + label +
                        (report.exact ? juce::String() : juce::String(": the load is not exact (leaves this build lacks)"))
                     : juce::String::fromUTF8(stored_->name.c_str()) + " (" + label + ") plays: a session restored it";
  Refresh();
  Relint();
  ApplyMonitorTrim();
  return true;
}

// ── The working document ─────────────────────────────────────────────────────────────────

// A factory mode the processor plays with no document open: a session restored it (FMOD,
// StateCodec.h). Its document opens as the stored version without a load, once per load; the
// working version is what plays.
bool CurationSession::AdoptPlaying() {
  const uint32_t serial = processor_.LoadSerial();
  if (serial == adoptSerial_) return false;
  adoptSerial_                = serial;
  const PresetSource source   = processor_.CurrentSource();
  if (source.factory < 0 || static_cast<size_t>(source.factory) >= FactoryCount()) return false;
  const FactoryPackage&      f = Factory(static_cast<size_t>(source.factory));
  const std::vector<uint8_t> bytes(f.bytes, f.bytes + f.size);
  const bsc::DecompileResult d = bsc::Decompile(bytes.data(), bytes.size());
  juce::String               error;
  return d.ok && LoadText(d.json, {juce::File(), source.factory}, true, &bytes, &error, false);
}

bool CurationSession::Refresh() {
  bool changed = false;
  // Reap finished workers; apply a level match that came in.
  for (auto it = workers_.begin(); it != workers_.end();) {
    if ((*it)->done.load(std::memory_order_acquire)) {
      (*it)->thread.join();
      it = workers_.erase(it);
    } else {
      ++it;
    }
  }
  {
    LevelMatch m;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      m = match_;
    }
    if (m.state != applied_.state || Bits(m.trimStored) != Bits(applied_.trimStored) ||
        Bits(m.trimWorking) != Bits(applied_.trimWorking)) {
      ApplyMonitorTrim();
      changed = true;
    }
  }
  if (!open_) return AdoptPlaying() || changed;
  if (side_ == Side::Working) {
    // A host that recalled a session (or anything else that loaded another mode) replaced the
    // document: it no longer plays. A factory mode the recall played opens in its place.
    const ModeState now = processor_.CurrentMode();
    if (std::memcmp(&now.mode, &storedState_->mode, sizeof(ModeBlob)) != 0) {
      CloseDocument();
      message_ = "The document closed: the plugin now plays another preset (a session recall?)";
      AdoptPlaying();
      return true;
    }
    const auto            preset = processor_.CurrentPreset();
    std::vector<uint32_t> bits;
    bits.reserve(kNumLeafParams + kMaxMacros + detached_.size() + 1);
    for (size_t i = 0; i < kNumLeafParams; ++i) bits.push_back(Bits(preset->leaves[i].value));
    for (size_t k = 0; k < kMaxMacros; ++k) bits.push_back(Bits(processor_.Macro(MacroId(k)).Plain()));
    bits.push_back(static_cast<uint32_t>(detached_.size()));
    bits.insert(bits.end(), detached_.begin(), detached_.end());
    // The live performance, which Save captures: a change of it alone (a tap, a host's tempo)
    // re-reads what Save will store, and leaves the level match as it is.
    const PerformanceState          live = processor_.LivePerformance();
    const std::array<uint32_t, 2> perf = {
        live.usPerQuarter, static_cast<uint32_t>(live.timeMode) | static_cast<uint32_t>(live.subdiv) << 8};
    if (bits != seenBits_) {
      seenBits_ = std::move(bits);
      seenPerformance_ = perf;
      RebuildWorking();
      lastChangeMs_ = NowMs();
      lintStale_    = true;
      matchStale_   = matchLevel_;
      changed       = true;
    } else if (perf != seenPerformance_) {
      seenPerformance_ = perf;
      RebuildWorking();
      lastChangeMs_ = NowMs();
      lintStale_    = true;
      changed       = true;
    }
  }
  const uint32_t quiet = NowMs() - lastChangeMs_;
  if (lintStale_ && quiet >= kLintSettleMs) {
    Relint();
    changed = true;
  }
  bool measuring = false;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    measuring = match_.state == LevelMatch::State::Measuring;
  }
  if (matchStale_ && !measuring && quiet >= kMatchSettleMs) StartLevelMatch();
  return changed;
}

void CurationSession::RebuildWorking() {
  auto           d      = std::make_unique<bsc::Document>(*stored_);
  PresetState&   s      = *d->state;
  const auto     preset = processor_.CurrentPreset();
  for (uint32_t k = 0; k < s.leafCount && k < PresetState::kMaxLeaves; ++k) {
    const size_t i = LeafIndex(s.leaves[k].id);
    if (i < kNumLeafParams) s.leaves[k].value = preset->leaves[i].value;
  }
  for (size_t k = 0; k < kMaxMacros; ++k) {
    if (float* p = Position(s, MacroId(k))) *p = processor_.Macro(MacroId(k)).Plain();
  }
  d->omittedPositions.clear();  // the app has every position
  d->detached = detached_;
  // Dirty: the content (leaves, positions, detached) differs from the stored document's.
  const PresetState& t = *stored_->state;
  bool               differs = d->detached != stored_->detached || s.leafCount != t.leafCount;
  for (uint32_t k = 0; !differs && k < s.leafCount; ++k) differs = Bits(s.leaves[k].value) != Bits(t.leaves[k].value);
  for (uint32_t k = 0; !differs && k < s.control.macroCount; ++k) {
    differs = Bits(s.control.positions[k].position) != Bits(t.control.positions[k].position);
  }
  dirty_ = differs;
  working_ = std::move(d);
  pending_.clear();
  bsc::Document derived(*working_);
  bsc::Derive(&derived, false, &pending_);
  // The performance Save captures (clock.md §10.3), apart from the edits: what plays, not what
  // the curator changed in the document.
  pendingPerformance_.clear();
  const PerformanceState& was  = t.performance;
  const PerformanceState  live = processor_.LivePerformance();
  static constexpr const char* kSubdivs[] = {"tap", "x1/4", "x1/2", "x2", "x4", "x8"};
  static constexpr const char* kModes[]   = {"free", "subdiv", "tempo"};
  const auto subdivName = [](Subdivision v) {
    const auto i = static_cast<size_t>(v);
    return i < 6u ? kSubdivs[i] : "?";
  };
  const auto modeName = [](brainscape::TimeMode v) {
    const auto i = static_cast<size_t>(v);
    return i < 3u ? kModes[i] : "?";
  };
  if (live.usPerQuarter != was.usPerQuarter) {
    pendingPerformance_.push_back("performance.tempo_us_per_quarter: " + std::to_string(was.usPerQuarter) + " -> " +
                                  std::to_string(live.usPerQuarter) + " (the live tempo)");
  }
  if (live.subdiv != was.subdiv) {
    pendingPerformance_.push_back(std::string("performance.subdiv: ") + subdivName(was.subdiv) + " -> " +
                                  subdivName(live.subdiv) + " (the live Subdiv)");
  }
  if (live.timeMode != was.timeMode) {
    pendingPerformance_.push_back(std::string("performance.time_mode: ") + modeName(was.timeMode) + " -> " +
                                  modeName(live.timeMode) + " (the live time mode)");
  }
}

std::unique_ptr<bsc::Document> CurationSession::SaveDocument(std::vector<std::string>* derived) const {
  auto d = std::make_unique<bsc::Document>(*working_);
  bsc::Derive(d.get(), false, derived);
  // Saving captures the performance (clock.md §10.3): the live committed tempo in whole µs, the
  // live Subdiv and time mode; `reverse` stays the document's until global reverse lands.
  const PerformanceState live     = processor_.LivePerformance();
  d->state->performance.usPerQuarter = live.usPerQuarter;
  d->state->performance.subdiv       = live.subdiv;
  d->state->performance.timeMode     = live.timeMode;
  return d;
}

// The compiler's errors on what Save would write, then its lint (factory rules for a factory
// id: L4 and L7-L9 are errors there).
void CurationSession::Relint() {
  lintStale_ = false;
  if (!open_ || working_ == nullptr) return;
  const auto               d = SaveDocument(nullptr);
  const bsc::CompileResult r = bsc::CompileDocument(*d);
  std::vector<bsc::Finding> all = r.findings;
  bsc::LintOptions          options;
  options.factory = IsFactory(*d);
  for (bsc::Finding& f : bsc::Lint(*d, options)) all.push_back(std::move(f));
  findings_ = ErrorsFirst(std::move(all));
}

// ── Macros, leaves, detached ─────────────────────────────────────────────────────────────

bool CurationSession::MacroDefined(ParamId macro) const {
  if (!open_) return false;
  const MacroTable& t = Mode().macros;
  for (uint32_t k = 0; k < t.macroCount; ++k) {
    if (t.macros[k].id == static_cast<uint32_t>(macro)) return true;
  }
  return false;
}

juce::String CurationSession::MacroName(ParamId macro) const {
  const auto k = static_cast<uint32_t>(macro) - static_cast<uint32_t>(ParamId::MacroActivity);
  if (open_ && k < kMaxMacros && !stored_->displayName[k].empty()) {
    return juce::String::fromUTF8(stored_->displayName[k].c_str());
  }
  return FindParamDisplay(macro)->title;
}

std::vector<CurationSession::Target> CurationSession::TargetsOfMacro(ParamId macro) const {
  std::vector<Target> out;
  if (!open_) return out;
  const MacroTable& t = Mode().macros;
  for (uint32_t k = 0; k < t.macroCount; ++k) {
    const MacroDef& md = t.macros[k];
    if (md.id != static_cast<uint32_t>(macro)) continue;
    for (uint32_t i = 0; i < md.count; ++i) {
      const MacroTarget& g = t.targets[md.first + i];
      out.push_back({macro, i, g.lo, g.hi, g.inLo, g.inHi, g.curve});
    }
  }
  return out;
}

std::vector<CurationSession::Target> CurationSession::TargetsOf(ParamId leaf) const {
  std::vector<Target> out;
  if (!open_) return out;
  const MacroTable& t = Mode().macros;
  for (uint32_t k = 0; k < t.macroCount; ++k) {
    const MacroDef& md = t.macros[k];
    for (uint32_t i = 0; i < md.count; ++i) {
      const MacroTarget& g = t.targets[md.first + i];
      if (g.param == static_cast<uint32_t>(leaf)) {
        out.push_back({static_cast<ParamId>(md.id), i, g.lo, g.hi, g.inLo, g.inHi, g.curve});
      }
    }
  }
  return out;
}

bool CurationSession::IsDetached(ParamId leaf) const {
  return std::binary_search(detached_.begin(), detached_.end(), static_cast<uint32_t>(leaf));
}

void CurationSession::SetDetached(ParamId leaf, bool detached) {
  const auto id = static_cast<uint32_t>(leaf);
  const auto at = std::lower_bound(detached_.begin(), detached_.end(), id);
  const bool is = at != detached_.end() && *at == id;
  if (detached == is || !IsLeaf(leaf)) return;
  if (detached) {
    detached_.insert(at, id);
  } else {
    detached_.erase(at);
  }
  Refresh();
}

// What `d` holds goes to the processor: each leaf whose mirror differs as a SetParam, each
// macro position as its mirror only (a pickup reference, never a MacroMove).
void CurationSession::ApplyLeaves(const bsc::Document& d) {
  const PresetState& s = *d.state;
  for (uint32_t k = 0; k < s.leafCount && k < PresetState::kMaxLeaves; ++k) {
    if (!IsLeaf(s.leaves[k].id)) continue;
    BrainscapeParam& p = processor_.Param(static_cast<ParamId>(s.leaves[k].id));
    if (Bits(p.Plain()) != Bits(s.leaves[k].value)) p.SetPlainNotifyingHost(s.leaves[k].value);
  }
  for (uint32_t k = 0; k < s.control.macroCount && k < kMaxMacros; ++k) {
    const auto id = static_cast<ParamId>(s.control.positions[k].macroId);
    if (!BrainscapeProcessor::IsMacroRow(id)) continue;
    BrainscapeParam& p = processor_.Macro(id);
    if (Bits(p.Plain()) == Bits(s.control.positions[k].position)) continue;
    p.StoreMirror(s.control.positions[k].position);
    p.setValueNotifyingHost(p.getValue());  // the host sees it; the echo posts nothing
  }
}

std::vector<std::string> CurationSession::SolvePositions(ParamId macro, ParamId fromLeaf) {
  std::vector<std::string> log;
  if (!open_ || side_ != Side::Working) return log;
  Refresh();
  bsc::Document d(*working_);
  if (static_cast<uint32_t>(macro) == 0u) {
    bsc::Derive(&d, true, &log);
  } else {
    PresetState&      s  = *d.state;
    const MacroTable& t  = s.mode.macros;
    float*            at = Position(s, macro);
    for (uint32_t k = 0; at != nullptr && k < t.macroCount; ++k) {
      const MacroDef& md = t.macros[k];
      if (md.id != static_cast<uint32_t>(macro)) continue;
      // The target solved from: `fromLeaf`'s, else the first that is not detached and not a
      // single value.
      const bool fromGiven = static_cast<uint32_t>(fromLeaf) != 0u;
      for (uint32_t i = 0; i < md.count; ++i) {
        const MacroTarget& g = t.targets[md.first + i];
        if (fromGiven && g.param != static_cast<uint32_t>(fromLeaf)) continue;
        if (IsDetached(static_cast<ParamId>(g.param)) || Bits(g.lo) == Bits(g.hi)) continue;
        const uint32_t before = Bits(*at);
        const uint32_t solved = bsc::SolvePosition(s.mode, md.id, i, d.LeafBits(g.param), before);
        *at                   = bsc::FloatOf(solved);
        if (solved != before) {
          log.push_back(std::string("controls.macro_positions.") + bsc::MacroName(md.id) + ": " +
                        bsc::NumberText(before) + " -> " + bsc::NumberText(solved) + " (from " +
                        FindParam(static_cast<ParamId>(g.param))->name + ")");
        }
        break;
      }
      // Its targets follow the solved position, as Derive writes them.
      PresetLeaf   out[kMaxMacroTargets];
      const size_t n = EvalMacro(s.mode, macro, *at, out, kMaxMacroTargets);
      for (size_t j = 0; j < n; ++j) {
        if (IsDetached(static_cast<ParamId>(out[j].id)) || !bsc::ElementPresent(s.mode, out[j].id)) continue;
        if (d.LeafBits(out[j].id) != Bits(out[j].value)) {
          log.push_back(std::string(FindParam(static_cast<ParamId>(out[j].id))->name) + ": " +
                        bsc::NumberText(d.LeafBits(out[j].id)) + " -> " + bsc::NumberText(Bits(out[j].value)));
        }
        d.SetLeafBits(out[j].id, Bits(out[j].value));
      }
    }
  }
  ApplyLeaves(d);
  seenBits_.clear();
  Refresh();
  message_ = log.empty() ? juce::String("Solve: every position already lands nearest its leaf")
                         : "Solved " + juce::String(static_cast<int>(log.size())) + " change(s)";
  return log;
}

// ── Saving ───────────────────────────────────────────────────────────────────────────────

CurationSession::SaveResult CurationSession::Save() {
  if (!open_) return {};
  if (factory_ >= 0) {  // built in: nothing on disk to write back to
    SaveResult result;
    result.message = message_ = "A factory mode is built in: Save as... writes a copy, which then plays";
    return result;
  }
  return WriteTo(jsonFile_, writesPackage_);
}

CurationSession::SaveResult CurationSession::SaveAs(const juce::File& json, bool withPackage) {
  if (!open_) return {};
  return WriteTo(json.withFileExtension(".json"), withPackage);
}

CurationSession::SaveResult CurationSession::WriteTo(const juce::File& json, bool withPackage) {
  SaveResult result;
  if (side_ == Side::Stored) SetSide(Side::Working);  // A is read-only: B is what saves
  Refresh();
  const auto               d = SaveDocument(&result.derived);
  const bsc::CompileResult r = bsc::CompileDocument(*d);
  juce::String             error;
  if (!r.ok) {
    // A document that does not compile is saved as JSON only (§9.1), its stamp as it was.
    const std::string text = bsc::FormatDocument(*d);
    result.written         = WriteBytes(json, text.data(), text.size(), &error);
    result.message = result.written ? "Saved " + json.getFileName() + " as JSON only: it does not compile (" +
                                          FirstError(r.findings) + ")"
                                    : error;
    message_ = result.message;
    Relint();
    return result;
  }
  bool mixed = false;
  if (withPackage ? !WritePair(json, r.json, json.withFileExtension(".bsp"), r.package, &error, &mixed)
                  : !WriteBytes(json, r.json.data(), r.json.size(), &error)) {
    result.message = message_ = error;
    return result;
  }
  result.written  = true;
  result.compiled = true;
  // The stored version is now what was written, and what plays: a Spillover load of the
  // package with Trails, the positions it stores being the knobs' (§9.1).
  auto                      stored = std::make_unique<bsc::Document>();
  std::vector<bsc::Finding> found;
  bsc::DecodedPackage       p = bsc::DecodePackage(r.package.data(), r.package.size());
  if (!bsc::ReadDocumentText(r.json, {}, stored.get(), &found) || !p.ok) {
    result.message = message_ = "Saved, but the written document does not read back: " + FirstError(found);
    return result;
  }
  processor_.LoadPresetState(*p.state, nullptr, {-1, juce::String::fromUTF8(stored->name.c_str()), stored->family});
  factory_       = -1;  // a copy of a factory mode is a document file from now on
  source_        = json;
  jsonFile_      = json;
  writesPackage_ = withPackage;
  stampCurrent_  = true;
  canonical_     = true;
  stored_        = std::move(stored);
  storedState_   = std::move(p.state);
  detached_      = stored_->detached;
  seenBits_.clear();
  ResetMatch();
  matchStale_ = matchLevel_;
  Refresh();
  Relint();
  result.message = "Saved " + json.getFileName() + (withPackage ? " and its package" : "") + ", sound_hash " +
                   juce::String(bsc::Hex(r.soundHash.bytes, 4)) +
                   (result.derived.empty() ? juce::String()
                                           : ", " + juce::String(static_cast<int>(result.derived.size())) +
                                                 " leaves derived from the knobs");
  message_ = result.message;
  return result;
}

// ── A/B ──────────────────────────────────────────────────────────────────────────────────

bool CurationSession::SetSide(Side side) {
  if (!open_ || side == side_) return false;
  if (side == Side::Stored) {
    Refresh();
    workingSnapshot_ = processor_.CurrentPreset();
    if (!processor_.LoadPresetState(*storedState_, nullptr, Source())) {
      workingSnapshot_.reset();
      return false;
    }
  } else {
    if (workingSnapshot_ == nullptr || !processor_.LoadPresetState(*workingSnapshot_, nullptr, Source())) return false;
    workingSnapshot_.reset();
    seenBits_.clear();
  }
  side_ = side;
  ApplyMonitorTrim();
  if (side_ == Side::Working) Refresh();
  return true;
}

void CurationSession::SetMatchLevel(bool on) {
  matchLevel_ = on;
  matchStale_ = on;
  if (!on) ResetMatch();
  lastChangeMs_ = NowMs() - kMatchSettleMs;  // measure at once
  ApplyMonitorTrim();
}

void CurationSession::SetInputClass(bsa::InputClass c) {
  if (c == inputClass_) return;
  inputClass_ = c;
  matchStale_ = matchLevel_;
}

CurationSession::LevelMatch CurationSession::GetLevelMatch() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return match_;
}

void CurationSession::ApplyMonitorTrim() {
  LevelMatch m;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    m = match_;
  }
  applied_ = m;
  float trim = 0.f;
  if (open_ && matchLevel_ && m.state == LevelMatch::State::Ready) {
    trim = side_ == Side::Stored ? m.trimStored : m.trimWorking;
  }
  processor_.SetMonitorTrimDb(trim);
}

// Both versions on the class input, from the exact-restart state, measured as integrated
// K-weighted loudness over the span the input sounds in.
void CurationSession::StartLevelMatch() {
  matchStale_ = false;
  if (!open_ || !matchLevel_ || storedState_ == nullptr) return;
  std::shared_ptr<const PresetState> stored = std::make_shared<PresetState>(*storedState_);
  std::shared_ptr<const PresetState> working =
      side_ == Side::Working ? std::shared_ptr<const PresetState>(processor_.CurrentPreset())
                             : std::make_shared<PresetState>(*workingSnapshot_);
  const bsa::Vector vector = bsa::ClassVector(inputClass_);
  uint64_t          gen    = 0;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    match_.state = LevelMatch::State::Measuring;
    gen          = ++matchGen_;
  }
  auto worker    = std::make_unique<Worker>();
  Worker* w      = worker.get();
  worker->thread = std::thread([this, w, stored, working, vector, gen] {
    LevelMatch      m;
    bsa::Renderer   renderer;
    const bsa::Input in = bsa::VectorInput(vector);
    bool            ok  = renderer.ok();
    double          lufs[2] = {0.0, 0.0};
    const PresetState* presets[2] = {stored.get(), working.get()};
    for (int k = 0; ok && k < 2; ++k) {
      bsa::RenderRequest rq;
      rq.preset = presets[k];
      rq.input  = &in.audio;
      rq.cancel = &cancel_;
      bsa::RenderResult rr;
      ok      = renderer.Render(rq, &rr);
      lufs[k] = ok ? bsa::Loudness(rr.out).Integrated(0, in.signalFrames) : 0.0;
    }
    if (ok && lufs[0] > bsa::kSilentDb && lufs[1] > bsa::kSilentDb) {
      m.state         = LevelMatch::State::Ready;
      m.storedLufs    = lufs[0];
      m.workingLufs   = lufs[1];
      const double d  = lufs[1] - lufs[0];  // B over A
      m.trimWorking   = d > 0.0 ? static_cast<float>(-d) : 0.f;
      m.trimStored    = d < 0.0 ? static_cast<float>(d) : 0.f;
    } else {
      m.state = LevelMatch::State::Failed;
    }
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (gen == matchGen_) match_ = m;  // else a load, a save or the switch voided it
    }
    w->done.store(true, std::memory_order_release);
  });
  workers_.push_back(std::move(worker));
}

// ── Render ───────────────────────────────────────────────────────────────────────────────

bool CurationSession::StartRender(const RenderRequest& request, juce::String* error) {
  juce::String  local;
  juce::String& err = error != nullptr ? *error : local;
  if (!open_) {
    err = "No document is open";
    return false;
  }
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (render_.state == RenderStatus::State::Running) {
      err = "A render is already running";
      return false;
    }
  }
  Refresh();
  const auto               d = SaveDocument(nullptr);
  const bsc::CompileResult r = bsc::CompileDocument(*d);
  if (!r.ok) {
    err = "The document does not compile: " + FirstError(r.findings);
    return false;
  }
  bsc::DecodedPackage p = bsc::DecodePackage(r.package.data(), r.package.size());
  if (!p.ok) {
    err = juce::String(bsc::DescribeDiagnostic(p.diagnostic));
    return false;
  }
  auto preset                  = std::make_shared<bsa::Preset>();
  preset->identity.package     = true;
  preset->identity.id          = d->id;
  preset->identity.name        = d->name;
  preset->identity.family      = FamilyName(d->family);
  preset->identity.source      = factory_ >= 0 ? "firmware/factory/" + std::string(Factory(static_cast<size_t>(factory_)).path)
                                               : jsonFile_.getFullPathName().replaceCharacter('\\', '/').toStdString();
  preset->identity.soundRev    = p.info.soundRev;
  preset->identity.soundHash   = bsc::Hex(r.soundHash.bytes, 32);
  preset->identity.controlHash = bsc::Hex(r.controlHash.bytes, 32);
  preset->identity.packageHash = bsc::Hex(r.packageHash.bytes, 32);
  preset->state                = std::shared_ptr<const PresetState>(std::move(p.state));
  preset->declare.inputClass   = inputClass_;
  bsa::SuiteOptions options;
  options.metrics = true;
  options.wav     = true;
  options.cancel  = &cancel_;
  options.outDir  = request.outDir.getFullPathName().toStdString();
  options.scripts.clear();
  if (request.allScripts) {
    for (const char* s : bsa::kScriptNames) options.scripts.push_back(s);
  } else {
    options.scripts.push_back("S0");
  }
  const juce::File dir = request.outDir.getChildFile(juce::String(bsa::PresetDir(*preset)));
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    render_         = RenderStatus{};
    render_.state   = RenderStatus::State::Running;
    render_.dir     = dir;
    render_.message = juce::String("Rendering ") + (request.allScripts ? "S0-S11" : "S0") + " of " + d->id +
                      " with the pre-screen...";
  }
  auto worker    = std::make_unique<Worker>();
  Worker* w      = worker.get();
  worker->thread = std::thread([this, w, preset, options, dir] {
    const auto      start = std::chrono::steady_clock::now();
    RenderStatus    s;
    s.dir = dir;
    bsa::Renderer   renderer;
    if (!renderer.ok()) {
      s.state   = RenderStatus::State::Failed;
      s.message = "The render engine could not be set up";
    } else {
      const bsa::SuiteResult res = bsa::RunSuite(renderer, *preset, {preset.get()}, options);
      s.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      if (res.cancelled) {
        s.state   = RenderStatus::State::Failed;
        s.message = "Stopped";
        const std::lock_guard<std::mutex> lock(mutex_);
        render_ = s;
        w->done.store(true, std::memory_order_release);
        return;
      }
      s.renders = static_cast<int>(res.renders.size());
      s.summary = juce::String(bsa::Summary(*preset, res));
      for (const bsa::Check& c : res.checks) {
        if (c.verdict == "FAIL") s.failures.add(juce::String(c.name + ": " + c.detail));
      }
      for (const bsa::Rendered& x : res.renders) {
        if (!x.error.empty()) s.failures.add(juce::String(x.plan.name + ": " + x.error));
      }
      s.state   = res.ok ? RenderStatus::State::Done : RenderStatus::State::Failed;
      s.message = juce::String(s.renders) + " renders in " + juce::String(s.seconds, 1) + " s; " +
                  (s.failures.isEmpty() ? juce::String("pre-screen passed")
                                        : juce::String(s.failures.size()) + " pre-screen failure(s)");
    }
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      render_ = s;
    }
    w->done.store(true, std::memory_order_release);
  });
  workers_.push_back(std::move(worker));
  return true;
}

CurationSession::RenderStatus CurationSession::GetRender() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return render_;
}

}  // namespace brainscape::plugin
