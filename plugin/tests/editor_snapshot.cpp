// Renders the editor offscreen (Component::createComponentSnapshot) to PNG files, so the
// layout can be checked without a display: default, minimum and large sizes, a HiDPI
// frame, a frame at 44.1 kHz with freeze engaged and the restart option on, and the
// Standalone's editor after an audition render.
//   brainscape_editor_snapshot <output directory>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "TestSupport.h"

using namespace brainscape;
using namespace brainscape::plugin;

namespace {

// Runs the pluck generator through the engine so the meters and onset LED have something
// to show.
void Play(BrainscapeProcessor& proc, double rate, double seconds) {
  const int                block = static_cast<int>(rate / 100.0);
  juce::AudioBuffer<float> buffer(2, block);
  juce::MidiBuffer         midi;
  for (int done = 0; done < static_cast<int>(rate * seconds); done += block) {
    buffer.clear();
    proc.processBlock(buffer, midi);
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
  proc.Param(ParamId::OnsetTrigger).SetPlainNotifyingHost(1.0f);
  proc.Param(ParamId::FilterCutoffHz).SetPlainNotifyingHost(3200.0f);
  proc.Param(ParamId::FilterMorph).SetPlainNotifyingHost(0.4f);

  std::unique_ptr<juce::AudioProcessorEditor> owned(proc.createEditor());
  auto* editor = dynamic_cast<BrainscapeEditor*>(owned.get());
  if (editor == nullptr) return 1;

  bool ok = true;
  Play(proc, 48000.0, 1.2);
  ok &= Snapshot(*editor, dir, "editor-default", BrainscapeEditor::kDefaultWidth, BrainscapeEditor::kDefaultHeight);
  Play(proc, 48000.0, 0.3);
  ok &= Snapshot(*editor, dir, "editor-minimum", BrainscapeEditor::kMinWidth, BrainscapeEditor::kMinHeight);
  ok &= Snapshot(*editor, dir, "editor-large", 1680, 1000);
  ok &= Snapshot(*editor, dir, "editor-default-2x", BrainscapeEditor::kDefaultWidth,
                 BrainscapeEditor::kDefaultHeight, 2.0f);

  proc.prepareToPlay(44100.0, 441);
  proc.Freeze().setValueNotifyingHost(1.0f);
  WrapperSettings settings = proc.GetSettings();
  settings.restartOnStart  = true;
  proc.SetSettings(settings);
  Play(proc, 44100.0, 0.5);
  ok &= Snapshot(*editor, dir, "editor-44k1-frozen", BrainscapeEditor::kDefaultWidth,
                 BrainscapeEditor::kDefaultHeight);
  owned.reset();

  // The Standalone's test-input panel offers the audition render instead.
  juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Standalone);
  BrainscapeProcessor app;
  juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
  app.prepareToPlay(48000.0, 480);
  app.GetTestInput().SetSource(TestInput::Source::Pluck);
  app.Param(ParamId::TransposeSt).SetPlainNotifyingHost(7.0f);
  app.Param(ParamId::ReverbMix).SetPlainNotifyingHost(0.3f);
  const juce::File take = dir.getChildFile("audition-take.wav");
  juce::String     error;
  ok &= app.StartAudition(take, error);
  for (int i = 0; i < 3000 && app.GetAudition().state == AuditionJob::State::Running; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ok &= app.GetAudition().state == AuditionJob::State::Done;
  std::unique_ptr<juce::AudioProcessorEditor> appOwned(app.createEditor());
  auto* appEditor = dynamic_cast<BrainscapeEditor*>(appOwned.get());
  if (appEditor == nullptr) return 1;
  Play(app, 48000.0, 0.5);
  ok &= Snapshot(*appEditor, dir, "editor-standalone-audition", BrainscapeEditor::kDefaultWidth,
                 BrainscapeEditor::kDefaultHeight);
  appOwned.reset();
  return ok ? 0 : 1;
}
