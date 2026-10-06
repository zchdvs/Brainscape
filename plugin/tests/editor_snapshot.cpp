// Renders the editor offscreen (Component::createComponentSnapshot) to PNG files, so the
// layout can be checked without a display: default, minimum and large sizes, a HiDPI
// frame, and a frame at 44.1 kHz with freeze engaged.
//   brainscape_editor_snapshot <output directory>
#include <cstdio>
#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"

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
  proc.Param(ParamId::PitchSt).SetPlainNotifyingHost(7.0f);
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
  Play(proc, 44100.0, 0.5);
  ok &= Snapshot(*editor, dir, "editor-44k1-frozen", BrainscapeEditor::kDefaultWidth,
                 BrainscapeEditor::kDefaultHeight);
  owned.reset();
  return ok ? 0 : 1;
}
