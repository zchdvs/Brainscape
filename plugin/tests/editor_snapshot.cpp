// Renders the editor offscreen (Component::createComponentSnapshot) to PNG files, so the
// layout can be checked without a display: both views (Pedal and Leaves) at the default,
// minimum and large sizes and a HiDPI frame, with no document and with a preset document open
// (a knob waiting for pickup, a hand-edited leaf Save will derive, Shift, the stored version
// playing for A/B, a finished render), a frame at 44.1 kHz with freeze engaged and the restart
// option on, a factory mode chosen from the header's Modes menu (and a reserve) with the menu
// itself drawn as the look and feel draws it, the Standalone's editor after an audition render,
// and the tempo strip (docs/design/clock.md §10.3): a clock preset's document under a tapped
// tempo with Subdiv and time mode set, the host's tempo followed, the strip at the minimum size,
// its settings menu, and the Standalone following a MIDI clock.
//   brainscape_editor_snapshot <output directory>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Curation.h"
#include "FactoryModes.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "TestSupport.h"

using namespace brainscape;
using namespace brainscape::plugin;

namespace {

// A host transport for the tempo frames: its tempo, and a position that advances with the
// blocks Play renders.
class SnapshotPlayHead final : public juce::AudioPlayHead {
 public:
  double bpm     = 128.0;
  double ppq     = 0.0;
  bool   playing = true;
  juce::Optional<PositionInfo> getPosition() const override {
    PositionInfo info;
    info.setIsPlaying(playing);
    info.setBpm(bpm);
    info.setPpqPosition(ppq);
    return info;
  }
};

// Runs the pluck generator through the engine so the meters and onset LED have something
// to show. With a playhead, its position follows the blocks; with `clockBpm`, a MIDI clock
// (F8 every 1/24 of a quarter, after an FA) arrives in the blocks' MIDI.
void Play(BrainscapeProcessor& proc, double rate, double seconds, SnapshotPlayHead* head = nullptr,
          double clockBpm = 0.0) {
  const int                block = static_cast<int>(rate / 100.0);
  juce::AudioBuffer<float> buffer(2, block);
  juce::MidiBuffer         midi;
  const double             tick = clockBpm > 0.0 ? rate * 60.0 / clockBpm / 24.0 : 0.0;
  double                   next = 0.0;
  for (int done = 0; done < static_cast<int>(rate * seconds); done += block) {
    buffer.clear();
    midi.clear();
    if (clockBpm > 0.0) {
      if (done == 0) midi.addEvent(juce::MidiMessage::midiStart(), 0);
      for (; next < done + block; next += tick) {
        midi.addEvent(juce::MidiMessage::midiClock(), static_cast<int>(next) - done);
      }
    }
    proc.processBlock(buffer, midi);
    if (head != nullptr) head->ppq += block * head->bpm / (60.0 * rate);
  }
}

bool Snapshot(BrainscapeEditor& editor, const juce::File& dir, const juce::String& name, int w, int h,
              float scale = 1.0f) {
  editor.setSize(w, h);
  editor.RefreshNow();
  const juce::Image image = editor.createComponentSnapshot(editor.getLocalBounds(), true, scale);
  const juce::File  file  = dir.getChildFile(name + ".png");
  file.deleteFile();
  juce::FileOutputStream out(file);
  juce::PNGImageFormat   png;
  const bool             ok = out.openedOk() && png.writeImageToStream(image, out);
  std::printf("%s %s (%dx%d @%.0fx, editor %dx%d)\n", ok ? "wrote" : "FAILED", file.getFullPathName().toRawUTF8(),
              image.getWidth(), image.getHeight(), static_cast<double>(scale), editor.getWidth(),
              editor.getHeight());
  return ok;
}

bool WriteImage(const juce::Image& image, const juce::File& dir, const juce::String& name) {
  const juce::File file = dir.getChildFile(name + ".png");
  file.deleteFile();
  juce::FileOutputStream out(file);
  juce::PNGImageFormat   png;
  const bool             ok = out.openedOk() && png.writeImageToStream(image, out);
  std::printf("%s %s (%dx%d)\n", ok ? "wrote" : "FAILED", file.getFullPathName().toRawUTF8(), image.getWidth(),
              image.getHeight());
  return ok;
}

// A popup menu as the look and feel draws it in its window, without one (PopupMenu's window
// needs a desktop): the background, then each item at its ideal height, section headers and
// separators included. *rowOfSubMenu: where the item that opens a submenu sits.
juce::Image DrawMenu(const juce::PopupMenu& menu, juce::LookAndFeel& lf, int minWidth, int itemHeight,
                     int* rowOfSubMenu) {
  const auto options = juce::PopupMenu::Options().withStandardItemHeight(itemHeight).withMinimumWidth(minWidth);
  const int  border  = lf.getPopupMenuBorderSizeWithOptions(options);
  struct Row {
    const juce::PopupMenu::Item* item;
    int                          height;
  };
  std::vector<Row> rows;
  int              width = minWidth, height = 2 * border;
  for (juce::PopupMenu::MenuItemIterator it(menu); it.next();) {
    const juce::PopupMenu::Item& item = it.getItem();
    int                          w = 80, h = 16;
    if (item.isSectionHeader) {
      lf.getIdealPopupMenuSectionHeaderSizeWithOptions(item.text, -1, w, h, options);
    } else {
      const juce::String measured =
          item.text + (item.shortcutKeyDescription.isEmpty() ? juce::String() : "   " + item.shortcutKeyDescription);
      lf.getIdealPopupMenuItemSizeWithOptions(measured, item.isSeparator, itemHeight, w, h, options);
    }
    width = std::max(width, w + 2 * border);
    height += h;
    rows.push_back({&item, h});
  }
  juce::Image image(juce::Image::ARGB, width, height, true);
  juce::Graphics g(image);
  lf.drawPopupMenuBackgroundWithOptions(g, width, height, options);
  int y = border;
  for (const Row& r : rows) {
    const juce::Rectangle<int> area(border, y, width - 2 * border, r.height);
    if (r.item->isSectionHeader) {
      lf.drawPopupMenuSectionHeaderWithOptions(g, area, r.item->text, options);
    } else {
      lf.drawPopupMenuItemWithOptions(g, area, false, *r.item, options);
    }
    if (r.item->subMenu != nullptr && rowOfSubMenu != nullptr) *rowOfSubMenu = y;
    y += r.height;
  }
  return image;  // the context flushes into the shared pixels as it goes
}

// The Modes menu and its reserves submenu beside it, as they open from the header.
bool SnapshotModesMenu(BrainscapeEditor& editor, const juce::File& dir, const juce::String& name) {
  const juce::PopupMenu menu  = editor.Modes().BuildMenu();
  juce::LookAndFeel&    lf    = editor.getLookAndFeel();
  const int             itemH = 24;
  int                   subY  = 0;
  const juce::Image     main  = DrawMenu(menu, lf, editor.Modes().getWidth(), itemH, &subY);
  juce::Image           sub;
  for (juce::PopupMenu::MenuItemIterator it(menu); it.next();) {
    if (it.getItem().subMenu != nullptr) sub = DrawMenu(*it.getItem().subMenu, lf, 160, itemH, nullptr);
  }
  const int      w = main.getWidth() + (sub.isValid() ? sub.getWidth() + 2 : 0) + 24;
  const int      h = std::max(main.getHeight(), subY + (sub.isValid() ? sub.getHeight() : 0)) + 24;
  juce::Image canvas(juce::Image::ARGB, w, h, true);
  {
    juce::Graphics g(canvas);  // flushed into the image when it goes (Direct2D on Windows)
    g.fillAll(palette::kBackground);
    g.drawImageAt(main, 12, 12);
    if (sub.isValid()) g.drawImageAt(sub, 12 + main.getWidth() + 2, 12 + subY);
  }
  return WriteImage(canvas, dir, name);
}

// Waits for the curation session's worker (a level match or a render) with the editor ticking.
template <typename Done>
bool WaitFor(BrainscapeEditor& editor, Done done) {
  for (int i = 0; i < 6000; ++i) {
    editor.RefreshNow();
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

}  // namespace

int main(int argc, char* argv[]) {
  brainscape::testing::ReportCrtErrorsOnStderr();
  juce::ScopedJuceInitialiser_GUI juce;
  const juce::File dir = argc > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(argv[1]))
                                  : juce::File::getCurrentWorkingDirectory();
  if (!dir.createDirectory()) {
    std::printf("cannot create %s\n", dir.getFullPathName().toRawUTF8());
    return 1;
  }

  BrainscapeProcessor proc;
  proc.prepareToPlay(48000.0, 480);
  proc.GetTestInput().SetSource(TestInput::Source::Pluck);
  proc.Param(ParamId::TransposeSt).SetPlainNotifyingHost(7.0f);
  proc.Param(ParamId::Feedback).SetPlainNotifyingHost(0.45f);
  proc.Param(ParamId::ReverbMix).SetPlainNotifyingHost(0.3f);
  proc.Param(ParamId::FilterCutoffHz).SetPlainNotifyingHost(3200.0f);
  proc.Param(ParamId::FilterMorph).SetPlainNotifyingHost(0.4f);

  std::unique_ptr<juce::AudioProcessorEditor> owned(proc.createEditor());
  auto* editor = dynamic_cast<BrainscapeEditor*>(owned.get());
  if (editor == nullptr) return 1;

  bool ok = true;
  Play(proc, 48000.0, 1.2);
  ok &= Snapshot(*editor, dir, "editor-default", BrainscapeEditor::kDefaultWidth, BrainscapeEditor::kDefaultHeight);
  editor->SetView(BrainscapeEditor::View::Leaves);
  Play(proc, 48000.0, 0.3);
  ok &= Snapshot(*editor, dir, "editor-leaves", BrainscapeEditor::kDefaultWidth, BrainscapeEditor::kDefaultHeight);
  ok &= Snapshot(*editor, dir, "editor-leaves-minimum", BrainscapeEditor::kMinWidth, BrainscapeEditor::kMinHeight);

  // A preset document, as the curation slice opens it (mode-compiler.md §9.1): a scratch copy of
  // the compiler's Engram example, so nothing under the source tree is written.
  const juce::File scratch = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                 .getNonexistentChildFile("brainscape-snapshot", "", false);
  ok &= scratch.createDirectory().wasOk();
  const juce::File doc = scratch.getChildFile("engram.json");
  ok &= juce::File(BRAINSCAPE_TEST_DATA).getChildFile("engram.json").copyFileTo(doc);
  juce::String error;
  if (!proc.Curation().Open(doc, &error)) {
    std::printf("cannot open %s: %s\n", doc.getFullPathName().toRawUTF8(), error.toRawUTF8());
    return 1;
  }
  proc.Curation().SetMatchLevel(true);
  Play(proc, 48000.0, 0.5);
  editor->SetView(BrainscapeEditor::View::Pedal);
  editor->RefreshNow();  // the GUI tick that sees the load locks the knobs for pickup
  // Time picks up where its stored 50 % is, then turns; Activity (stored at 0) waits for pickup.
  editor->Macros().Knob(3).MoveTo(0.62f);
  editor->Macros().Knob(0).MoveTo(0.35f);
  // A hand-edited leaf that Repeats targets: Save will derive it back.
  proc.Param(ParamId::DelayFb).SetPlainNotifyingHost(0.3f);
  Play(proc, 48000.0, 0.6);
  ok &= WaitFor(*editor, [&] {
    return proc.Curation().GetLevelMatch().state == CurationSession::LevelMatch::State::Ready &&
           !proc.Curation().MatchPending();
  });
  ok &= Snapshot(*editor, dir, "editor-pedal-document", BrainscapeEditor::kDefaultWidth,
                 BrainscapeEditor::kDefaultHeight);
  ok &= Snapshot(*editor, dir, "editor-pedal-minimum", BrainscapeEditor::kMinWidth, BrainscapeEditor::kMinHeight);
  ok &= Snapshot(*editor, dir, "editor-pedal-large", 1680, 1000);
  ok &= Snapshot(*editor, dir, "editor-pedal-2x", BrainscapeEditor::kDefaultWidth, BrainscapeEditor::kDefaultHeight,
                 2.0f);
  editor->SetView(BrainscapeEditor::View::Leaves);
  ok &= Snapshot(*editor, dir, "editor-leaves-document", BrainscapeEditor::kDefaultWidth,
                 BrainscapeEditor::kDefaultHeight);

  editor->SetView(BrainscapeEditor::View::Pedal);
  editor->Macros().Shift().setToggleState(true, juce::sendNotificationSync);
  editor->Macros().Shift().onClick();
  Play(proc, 48000.0, 0.2);
  ok &= Snapshot(*editor, dir, "editor-pedal-shift", BrainscapeEditor::kDefaultWidth,
                 BrainscapeEditor::kDefaultHeight);
  editor->Macros().Shift().setToggleState(false, juce::sendNotificationSync);
  editor->Macros().Shift().onClick();

  ok &= proc.Curation().SetSide(CurationSession::Side::Stored);
  Play(proc, 48000.0, 0.2);
  ok &= Snapshot(*editor, dir, "editor-pedal-a", BrainscapeEditor::kDefaultWidth, BrainscapeEditor::kDefaultHeight);
  ok &= proc.Curation().SetSide(CurationSession::Side::Working);

  CurationSession::RenderRequest rq;
  rq.outDir = scratch.getChildFile("renders");
  ok &= proc.Curation().StartRender(rq, &error);
  ok &= WaitFor(*editor, [&] {
    return proc.Curation().GetRender().state != CurationSession::RenderStatus::State::Running;
  });
  ok &= Snapshot(*editor, dir, "editor-pedal-rendered", BrainscapeEditor::kDefaultWidth,
                 BrainscapeEditor::kDefaultHeight);
  proc.Curation().Close();

  proc.prepareToPlay(44100.0, 441);
  proc.Freeze().setValueNotifyingHost(1.0f);
  WrapperSettings settings = proc.GetSettings();
  settings.restartOnStart  = true;
  proc.SetSettings(settings);
  Play(proc, 44100.0, 0.5);
  editor->SetView(BrainscapeEditor::View::Leaves);
  ok &= Snapshot(*editor, dir, "editor-44k1-frozen", BrainscapeEditor::kDefaultWidth,
                 BrainscapeEditor::kDefaultHeight);

  // A factory mode from the header's Modes menu, as its handler opens it (ModeMenu::Choose): Lull
  // playing with its document in the Pedal view, at the default and minimum sizes; the menu
  // itself; then a reserve.
  proc.Freeze().setValueNotifyingHost(0.0f);
  settings.restartOnStart = false;
  proc.SetSettings(settings);
  proc.prepareToPlay(48000.0, 480);
  Play(proc, 48000.0, 0.3);
  editor->SetView(BrainscapeEditor::View::Pedal);
  const int lull = FindFactory("factory.lull");
  ok &= lull >= 0 && editor->Modes().Choose(ModeMenu::kFactoryItem + lull);
  ok &= proc.CurrentSource().factory == lull;
  Play(proc, 48000.0, 0.8);
  ok &= Snapshot(*editor, dir, "editor-pedal-factory", BrainscapeEditor::kDefaultWidth,
                 BrainscapeEditor::kDefaultHeight);
  ok &= Snapshot(*editor, dir, "editor-pedal-factory-minimum", BrainscapeEditor::kMinWidth,
                 BrainscapeEditor::kMinHeight);
  editor->setSize(BrainscapeEditor::kDefaultWidth, BrainscapeEditor::kDefaultHeight);
  ok &= SnapshotModesMenu(*editor, dir, "editor-modes-menu");
  const int runaway = FindFactory("factory.runaway");
  ok &= runaway >= 0 && editor->Modes().Choose(ModeMenu::kFactoryItem + runaway);
  ok &= proc.CurrentSource().factory == runaway;  // Lull unedited: no discard question
  Play(proc, 48000.0, 0.5);
  ok &= Snapshot(*editor, dir, "editor-pedal-reserve", BrainscapeEditor::kDefaultWidth,
                 BrainscapeEditor::kDefaultHeight);
  ok &= Snapshot(*editor, dir, "editor-pedal-reserve-large", 1680, 1000);

  // The tempo strip (clock.md §10.3) over a preset that uses tempo: the golden corpus's
  // clock_hits document (stored at 140 BPM), the Subdiv at x2 in Subdiv time mode, three taps at
  // 100 BPM under the internal source; then the host's 128 BPM followed (TAP greyed), at the
  // default and minimum sizes; and the strip's settings menu.
  const juce::File clockDoc = scratch.getChildFile("clock_hits.json");
  ok &= juce::File(BRAINSCAPE_GOLDEN_PRESETS).getChildFile("clock_hits.json").copyFileTo(clockDoc);
  if (!proc.Curation().Open(clockDoc, &error)) {
    std::printf("cannot open %s: %s\n", clockDoc.getFullPathName().toRawUTF8(), error.toRawUTF8());
    return 1;
  }
  settings             = proc.GetSettings();
  settings.tempoSource = TempoSource::Internal;
  proc.SetSettings(settings);
  Play(proc, 48000.0, 0.3);
  editor->Tempo().SubdivSegment(3).onClick();
  editor->Tempo().TimeSegment(1).onClick();
  for (int tap = 0; tap < 3; ++tap) {
    proc.TapFromUi();
    Play(proc, 48000.0, 0.6);  // 100 BPM
  }
  Play(proc, 48000.0, 0.15);
  ok &= Snapshot(*editor, dir, "editor-tempo-internal", BrainscapeEditor::kDefaultWidth,
                 BrainscapeEditor::kDefaultHeight);
  SnapshotPlayHead host;
  settings.tempoSource = TempoSource::Host;
  proc.SetSettings(settings);
  proc.setPlayHead(&host);
  Play(proc, 48000.0, 0.5, &host);
  ok &= Snapshot(*editor, dir, "editor-tempo-host", BrainscapeEditor::kDefaultWidth,
                 BrainscapeEditor::kDefaultHeight);
  ok &= Snapshot(*editor, dir, "editor-tempo-host-minimum", BrainscapeEditor::kMinWidth,
                 BrainscapeEditor::kMinHeight);
  proc.setPlayHead(nullptr);
  editor->setSize(BrainscapeEditor::kDefaultWidth, BrainscapeEditor::kDefaultHeight);
  {
    juce::LookAndFeel& lf   = editor->getLookAndFeel();
    const juce::Image  menu = DrawMenu(editor->Tempo().SettingsMenu(), lf, 240, 24, nullptr);
    juce::Image        canvas(juce::Image::ARGB, menu.getWidth() + 24, menu.getHeight() + 24, true);
    {
      juce::Graphics g(canvas);
      g.fillAll(palette::kBackground);
      g.drawImageAt(menu, 12, 12);
    }
    ok &= WriteImage(canvas, dir, "editor-tempo-settings");
  }
  // Synced times (clock.md §5.3, §5.4): the corpus's sync_post document (a post delay synced to
  // 1/4, stored at 140 BPM) at x1/2 under the internal source: the strip's second line shows what
  // the echo plays, and the Leaves view dims post.delay.time_ms, which waits.
  const juce::File syncDoc = scratch.getChildFile("sync_post.json");
  ok &= juce::File(BRAINSCAPE_GOLDEN_PRESETS).getChildFile("sync_post.json").copyFileTo(syncDoc);
  if (!proc.Curation().Open(syncDoc, &error)) {
    std::printf("cannot open %s: %s\n", syncDoc.getFullPathName().toRawUTF8(), error.toRawUTF8());
    return 1;
  }
  settings.tempoSource = TempoSource::Internal;
  proc.SetSettings(settings);
  Play(proc, 48000.0, 0.3);
  editor->Tempo().SubdivSegment(1).onClick();
  Play(proc, 48000.0, 0.5);
  ok &= Snapshot(*editor, dir, "editor-tempo-synced", BrainscapeEditor::kDefaultWidth,
                 BrainscapeEditor::kDefaultHeight);
  editor->SetView(BrainscapeEditor::View::Leaves);
  ok &= Snapshot(*editor, dir, "editor-leaves-synced", BrainscapeEditor::kDefaultWidth,
                 BrainscapeEditor::kDefaultHeight);
  editor->SetView(BrainscapeEditor::View::Pedal);
  proc.Curation().Close();
  owned.reset();
  scratch.deleteRecursively();

  // The Standalone's test-input panel offers the audition render instead.
  juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Standalone);
  BrainscapeProcessor app;
  juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
  app.prepareToPlay(48000.0, 480);
  app.GetTestInput().SetSource(TestInput::Source::Pluck);
  app.Param(ParamId::TransposeSt).SetPlainNotifyingHost(7.0f);
  app.Param(ParamId::ReverbMix).SetPlainNotifyingHost(0.3f);
  const juce::File take = dir.getChildFile("audition-take.wav");
  ok &= app.StartAudition(take, error);
  for (int i = 0; i < 3000 && app.GetAudition().state == AuditionJob::State::Running; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ok &= app.GetAudition().state == AuditionJob::State::Done;
  std::unique_ptr<juce::AudioProcessorEditor> appOwned(app.createEditor());
  auto* appEditor = dynamic_cast<BrainscapeEditor*>(appOwned.get());
  if (appEditor == nullptr) return 1;
  Play(app, 48000.0, 0.5);
  appEditor->SetView(BrainscapeEditor::View::Leaves);
  ok &= Snapshot(*appEditor, dir, "editor-standalone-audition", BrainscapeEditor::kDefaultWidth,
                 BrainscapeEditor::kDefaultHeight);
  // The Standalone following a MIDI clock at 132 BPM from its MIDI input (clock.md §10.2): MIDI,
  // locked after 24 ticks.
  Play(app, 48000.0, 2.0, nullptr, 132.0);
  appEditor->SetView(BrainscapeEditor::View::Pedal);
  ok &= Snapshot(*appEditor, dir, "editor-tempo-midi", BrainscapeEditor::kDefaultWidth,
                 BrainscapeEditor::kDefaultHeight);
  {
    juce::LookAndFeel& lf   = appEditor->getLookAndFeel();
    const juce::Image  menu = DrawMenu(appEditor->Tempo().SettingsMenu(), lf, 240, 24, nullptr);
    juce::Image        canvas(juce::Image::ARGB, menu.getWidth() + 24, menu.getHeight() + 24, true);
    {
      juce::Graphics g(canvas);
      g.fillAll(palette::kBackground);
      g.drawImageAt(menu, 12, 12);
    }
    ok &= WriteImage(canvas, dir, "editor-tempo-settings-standalone");
  }
  appOwned.reset();
  return ok ? 0 : 1;
}
