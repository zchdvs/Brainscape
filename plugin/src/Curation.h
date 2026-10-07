#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <juce_core/juce_core.h>

#include "Document.h"  // brainscape_compiler: the preset document (mode-compiler.md §2)
#include "Suite.h"     // tools/audition: the scripts and the pre-screen (§11.3)
#include "brainscape/Params.h"
#include "brainscape/PresetState.h"

namespace brainscape::plugin {

class BrainscapeProcessor;

// The curation slice (docs/design/mode-compiler.md §9.1, §11.3): one preset document open in
// the app, auditioned live through the processor, edited with the macro knobs and the raw
// leaves, and saved back as canonical JSON through the compiler library. Message thread only,
// except the render and level-match workers, which own their own engines.
//
// The working document is the stored one with what plays now: the processor's leaf mirrors and
// its macro mirrors as the positions (pickup references, §3.5), and the detached set. Saving
// derives every targeted leaf that is not detached from its macro's position (EvalMacro, the
// derive rule), compiles, writes the stamped canonical JSON (and the package, when one sits
// beside it or the document came from one), and Spillover-loads what it wrote, so what plays is
// what was saved. A document that does not compile is written as JSON only, as the editor does.
class CurationSession {
 public:
  explicit CurationSession(BrainscapeProcessor& processor);
  ~CurationSession();
  CurationSession(const CurationSession&)            = delete;
  CurationSession& operator=(const CurationSession&) = delete;

  // ── The document ──────────────────────────────────────────────────────────────────────
  // Opens a schema-1 document (.json) or a package (.bsp: its JSON section, or the document
  // rebuilt from it) and plays it (BrainscapeProcessor::LoadPresetState: Spillover with Trails
  // while audio runs, Exact before). False, with *error and the findings, when it does not
  // read, compile or load; the open document stays then.
  bool Open(const juce::File& file, juce::String* error);
  // Re-opens the source from disk, dropping unsaved changes.
  bool Revert(juce::String* error);
  void Close();
  bool HasDocument() const { return open_; }
  // Where Save writes: the .json (for a .bsp source, the .json beside it), and the package.
  juce::File DocumentFile() const { return jsonFile_; }
  juce::File PackageFile() const { return jsonFile_.withFileExtension(".bsp"); }
  juce::File SourceFile() const { return source_; }
  bool       WritesPackage() const { return writesPackage_; }
  const bsc::Document&       Stored() const { return *stored_; }
  const bsc::Document&       Working() const { return *working_; }
  const brainscape::ModeBlob& Mode() const { return stored_->state->mode; }
  // The stamp on disk against this build: written, of this sound revision, and the hash the
  // document compiles to now.
  bool StampCurrent() const { return stampCurrent_; }

  // Message thread, from the editor's timer: rebuilds the working document from the mirrors when
  // a leaf, a position or the detached set changed, and re-lints it. Returns whether anything
  // changed. When the processor plays another mode than the document (a host recalled a
  // session), the document is closed.
  bool Refresh();
  // The working document's content differs from the stored one (leaves, positions, detached).
  bool Dirty() const { return dirty_; }
  // What Save's derive will change: one line per targeted leaf that is off its macro's value at
  // the macro's position ("post.delay.fb: 0.4 -> 0.45 (repeats at 0.5)").
  const std::vector<std::string>& PendingDerives() const { return pending_; }

  // Lint of the working document (L1-L9, factory rules for a "factory." id), and after a save
  // the compile errors too. Errors first.
  const std::vector<bsc::Finding>& Findings() const { return findings_; }
  juce::String                     LastMessage() const { return message_; }

  // ── Macros and leaves, for the views ───────────────────────────────────────────────────
  bool         MacroDefined(ParamId macro) const;
  // The document's display name for a macro, or the knob's own ("Activity").
  juce::String MacroName(ParamId macro) const;
  struct Target {
    ParamId macro;
    uint32_t index;  // within the macro's targets
    float    lo, hi, inLo, inHi, curve;
  };
  std::vector<Target> TargetsOf(ParamId leaf) const;     // the macros that move a leaf
  std::vector<Target> TargetsOfMacro(ParamId macro) const;

  // editor.detached (§3.5): a detached leaf keeps its value when Save derives.
  bool IsDetached(ParamId leaf) const;
  void SetDetached(ParamId leaf, bool detached);

  // "Solve position" (§3.5): each macro's position (or `macro`'s alone) from the leaf of its
  // first target that is not detached and not a single value (bsc::SolvePosition), the leaves
  // the solved positions derive to sent to the engine, the macro mirrors moved without a
  // MacroMove (pickup references). Returns a line per change.
  std::vector<std::string> SolvePositions(ParamId macro = static_cast<ParamId>(0));

  // ── Save ──────────────────────────────────────────────────────────────────────────────
  struct SaveResult {
    bool                     written  = false;
    bool                     compiled = false;
    std::vector<std::string> derived;  // the leaves derive changed
    juce::String             message;
  };
  SaveResult Save();
  // To another document file (its package beside it when `withPackage`); it becomes the source.
  SaveResult SaveAs(const juce::File& json, bool withPackage);

  // ── A/B against the stored version (§9.1) ───────────────────────────────────────────────
  // B is the working state, A the version on disk. Switching to A remembers what plays and
  // loads the stored package; switching back loads the remembered state. Both are Spillover
  // loads with Trails. Edits are for B: the views lock while A plays.
  enum class Side : uint8_t { Working, Stored };
  bool SetSide(Side side);
  Side GetSide() const { return side_; }
  // Level matching: both versions rendered offline on the class input (Plucks, or SoftNotes
  // for a pad mode) and measured as K-weighted loudness (tools/audition's Loudness); the louder
  // side is trimmed on the monitor output (BrainscapeProcessor::SetMonitorTrimDb), never in
  // a preset or a render.
  void SetMatchLevel(bool on);
  bool MatchLevel() const { return matchLevel_; }
  struct LevelMatch {
    enum class State : uint8_t { Off, Measuring, Ready, Failed } state = State::Off;
    double storedLufs  = 0.0;
    double workingLufs = 0.0;
    float  trimStored  = 0.f;  // dB, on A's output
    float  trimWorking = 0.f;  // dB, on B's output
  };
  LevelMatch GetLevelMatch() const;
  // The working state changed since the last measurement: a new one starts once the knobs rest.
  bool       MatchPending() const { return matchLevel_ && matchStale_; }

  // The mode's input class (§11.3): what the render scripts and the level match play into it,
  // Plucks for an attack mode, SoftNotes for a pad mode. Documents cannot declare it (the ratings
  // log does), so the curator picks it.
  void            SetInputClass(bsa::InputClass c);
  bsa::InputClass GetInputClass() const { return inputClass_; }

  // ── One-click offline render (tools/audition, §11.3) ────────────────────────────────────
  // The document as Save would write it, through the audition scripts with the pre-screen, on a
  // worker thread; WAVs, recipes and audition.json go to outDir/<id>/.
  struct RenderRequest {
    bool       allScripts = false;  // S0 only, or S0-S11
    juce::File outDir;
  };
  bool StartRender(const RenderRequest& request, juce::String* error);
  struct RenderStatus {
    enum class State : uint8_t { Idle, Running, Done, Failed } state = State::Idle;
    juce::String      message;   // one line for the panel
    juce::String      summary;   // tools/audition's text summary
    juce::StringArray failures;  // the pre-screen's FAIL lines
    juce::File        dir;       // where the renders went
    int               renders = 0;
    double            seconds = 0.0;
  };
  RenderStatus GetRender() const;
  // The default output directory: Music/Brainscape audition.
  static juce::File DefaultRenderDir();

 private:
  struct Worker;  // a job on its own thread

  bool LoadText(const std::string& text, const juce::File& source, bool fromPackage,
                const std::vector<uint8_t>* package, juce::String* error);
  void RebuildWorking();
  void Relint();
  std::unique_ptr<bsc::Document> SaveDocument(std::vector<std::string>* derived) const;
  SaveResult WriteTo(const juce::File& json, bool withPackage);
  void ApplyLeaves(const bsc::Document& d);
  void StartLevelMatch();
  void ResetMatch();
  void ApplyMonitorTrim();
  void JoinWorkers();

  BrainscapeProcessor&               processor_;
  bool                               open_ = false;
  juce::File                         source_, jsonFile_;
  bool                               writesPackage_ = false;
  bool                               stampCurrent_  = false;
  std::unique_ptr<bsc::Document>     stored_, working_;
  std::unique_ptr<PresetState>       storedState_;  // the stored package, decoded: A
  std::vector<uint32_t>              detached_;
  bool                               dirty_ = false;
  std::vector<std::string>           pending_;
  std::vector<bsc::Finding>          findings_, saveFindings_;
  juce::String                       message_;
  // What the last Refresh saw, to rebuild only on change.
  std::vector<uint32_t>              seenBits_;
  Side                               side_ = Side::Working;
  std::unique_ptr<PresetState>       workingSnapshot_;  // B while A plays

  bsa::InputClass                    inputClass_ = bsa::InputClass::Attack;
  bool                               matchLevel_ = false;
  bool                               matchStale_ = false;
  bool                               lintStale_  = false;
  uint32_t                           lastChangeMs_ = 0;
  LevelMatch                         applied_;  // what ApplyMonitorTrim last applied
  mutable std::mutex                 mutex_;  // guards match_, render_ and the workers' results
  LevelMatch                         match_;
  uint64_t                           matchGen_ = 0;  // a measurement's result counts only if
                                                     // nothing reset match_ since it started
  RenderStatus                       render_;
  std::vector<std::unique_ptr<Worker>> workers_;
};

}  // namespace brainscape::plugin
