// The companion app (companion §2.2): JUCE's standalone holder and window around the same
// processor as the plugins, with a shell that prefers the pedal's 48 kHz device rate.
// JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1 (set PUBLIC in plugin/CMakeLists.txt) makes
// JUCE's Standalone wrapper call this file's juce_CreateApplication.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#if JucePlugin_Build_Standalone

#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

#include "PluginProcessor.h"
#include "gui/BrainscapeLookAndFeel.h"

namespace brainscape::plugin {

namespace {

// The saved device rate wins over the preferred one on every launch after the first
// (companion §2.2), so the shell asks for 48 kHz again once the device is open. A device
// that cannot run at 48 kHz keeps its rate; the status line then says "Not pedal rate".
// Every device restart re-runs prepareToPlay, which re-derives the status.
void PreferPedalRate(juce::AudioDeviceManager& devices) {
  juce::AudioIODevice* device = devices.getCurrentAudioDevice();
  if (device == nullptr || device->getCurrentSampleRate() == BrainscapeProcessor::kPedalRate) return;
  if (!device->getAvailableSampleRates().contains(BrainscapeProcessor::kPedalRate)) return;
  auto setup       = devices.getAudioDeviceSetup();
  setup.sampleRate = BrainscapeProcessor::kPedalRate;
  devices.setAudioDeviceSetup(setup, true);
}

}  // namespace

class StandaloneApp final : public juce::JUCEApplication {
 public:
  StandaloneApp() {
    juce::PropertiesFile::Options options;
    options.applicationName     = "Brainscape";
    options.filenameSuffix      = ".settings";
    options.osxLibrarySubFolder = "Application Support";
#if JUCE_LINUX || JUCE_BSD
    options.folderName = "~/.config";
#endif
    properties_.setStorageParameters(options);
  }

  const juce::String getApplicationName() override { return "Brainscape"; }
  const juce::String getApplicationVersion() override { return JucePlugin_VersionString; }
  bool               moreThanOneInstanceAllowed() override { return true; }
  void               anotherInstanceStarted(const juce::String&) override {}

  void initialise(const juce::String&) override {
    juce::LookAndFeel::setDefaultLookAndFeel(&laf_);  // dark settings dialog and menus too
    juce::AudioDeviceManager::AudioDeviceSetup preferred;
    preferred.sampleRate = BrainscapeProcessor::kPedalRate;
    // Input stays muted until the user unmutes it in Options > Audio/MIDI Settings: JUCE's
    // default, and the feedback-safe one (companion §2.2).
    auto holder = std::make_unique<juce::StandalonePluginHolder>(properties_.getUserSettings(), false,
                                                                 juce::String(), &preferred);
    PreferPedalRate(holder->deviceManager);
    window_ = std::make_unique<juce::StandaloneFilterWindow>(getApplicationName(), palette::kBackground,
                                                             std::move(holder));
    window_->setVisible(true);
  }

  void shutdown() override {
    window_ = nullptr;
    properties_.saveIfNeeded();
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
  }

  void systemRequestedQuit() override {
    if (window_ != nullptr) window_->pluginHolder->savePluginState();
    if (juce::ModalComponentManager::getInstance()->cancelAllModalComponents()) {
      juce::Timer::callAfterDelay(100, [] {
        if (auto* app = juce::JUCEApplicationBase::getInstance()) app->systemRequestedQuit();
      });
    } else {
      quit();
    }
  }

 private:
  BrainscapeLookAndFeel                         laf_;
  juce::ApplicationProperties                   properties_;
  std::unique_ptr<juce::StandaloneFilterWindow> window_;
};

}  // namespace brainscape::plugin

juce::JUCEApplicationBase* juce_CreateApplication();
juce::JUCEApplicationBase* juce_CreateApplication() { return new brainscape::plugin::StandaloneApp(); }

#endif
