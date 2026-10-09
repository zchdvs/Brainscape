// Loads the built VST3 through JUCE's headless VST3 host and drives it as a DAW does: a
// session state goes in through IComponent::setState (with the controller's parameter
// echo, companion §5.3), is read back bit for bit, and audio runs in place in host blocks
// of varying size, zero-frame calls included. The output must equal the engine driven
// directly in 48-frame blocks. A first, small form of the plugin-format-parity leg
// (companion §3.4).
//   brainscape_vst3_host_check <path to Brainscape.vst3>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include "StateCodec.h"
#include "TestSupport.h"

using namespace brainscape;
using namespace brainscape::plugin;
using namespace brainscape::testing;

namespace {

int gFailures = 0;

void Check(bool ok, const juce::String& what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8());
  if (!ok) ++gFailures;
}

// The JUCE host keeps the plugin's IComponent state as base64 inside its own XML.
std::unique_ptr<juce::XmlElement> HostXml(juce::AudioPluginInstance& host) {
  juce::MemoryBlock mb;
  host.getStateInformation(mb);
  return juce::AudioProcessor::getXmlFromBinary(mb.getData(), static_cast<int>(mb.getSize()));
}

std::vector<uint8_t> ComponentState(juce::AudioPluginInstance& host) {
  const auto xml = HostXml(host);
  const auto* comp = xml != nullptr ? xml->getChildByName("IComponent") : nullptr;
  juce::MemoryBlock raw;
  if (comp == nullptr || !raw.fromBase64Encoding(comp->getAllSubText())) return {};
  const auto* p = static_cast<const uint8_t*>(raw.getData());
  return std::vector<uint8_t>(p, p + raw.getSize());
}

bool SetComponentState(juce::AudioPluginInstance& host, const std::vector<uint8_t>& blob) {
  const auto xml  = HostXml(host);
  auto*      comp = xml != nullptr ? xml->getChildByName("IComponent") : nullptr;
  if (comp == nullptr) return false;
  comp->deleteAllTextElements();
  comp->addTextElement(juce::MemoryBlock(blob.data(), blob.size()).toBase64Encoding());
  juce::MemoryBlock out;
  juce::AudioProcessor::copyXmlToBinary(*xml, out);
  host.setStateInformation(out.getData(), static_cast<int>(out.getSize()));
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::printf("usage: brainscape_vst3_host_check <Brainscape.vst3>\n");
    return 2;
  }
  ReportCrtErrorsOnStderr();
  juce::MessageManager::getInstance();  // this thread is the host's message thread
  {
    juce::AudioPluginFormatManager formats;
    juce::addHeadlessDefaultFormatsToManager(formats);
    juce::OwnedArray<juce::PluginDescription> types;
    for (auto* f : formats.getFormats()) {
      if (f->getName() == "VST3") f->findAllTypesForFile(types, juce::String(argv[1]));
    }
    Check(types.size() == 1, "one plugin type in " + juce::String(argv[1]));
    if (types.isEmpty()) return 1;

    juce::String error;
    auto         host = formats.createPluginInstance(*types[0], 48000.0, 512, error);
    Check(host != nullptr, "instantiated " + types[0]->name + (error.isEmpty() ? "" : ": " + error));
    if (host == nullptr) return 1;

    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add(juce::AudioChannelSet::stereo());
    layout.outputBuses.add(juce::AudioChannelSet::stereo());
    Check(host->setBusesLayout(layout), "stereo in, stereo out");
    layout.inputBuses.getReference(0) = juce::AudioChannelSet::mono();
    Check(host->checkBusesLayoutSupported(layout), "mono in, stereo out supported");
    layout.inputBuses.getReference(0) = juce::AudioChannelSet::stereo();

    bool haveDelay = false, haveFreeze = false;
    for (auto* p : host->getParameters()) {
      haveDelay |= p->getName(64) == "Delay time";
      haveFreeze |= p->getName(64) == "Freeze";
    }
    // The Leaf rows, freeze, the eight macros, the expression pedal, the effect volume
    // (mode-compiler.md §9.2), and the bypass JUCE's VST3 wrapper adds.
    Check(host->getParameters().size() == static_cast<int>(kNumLeafParams) + 12 && haveDelay && haveFreeze,
          juce::String(host->getParameters().size()) + " host parameters, engine and freeze present");

    // A session state with exact values: the record's two awkward ones (companion-app
    // record §2.5) on top of the busy preset.
    Preset preset = Busy();
    preset.push_back({ParamId::TransposeSt, 7.02f});
    preset.push_back({ParamId::FilterMorph, 0.4f});
    const Preset all = Complete(preset);
    WrapperState state{};
    for (size_t i = 0; i < kNumLeafParams; ++i) state.plain[i] = all[i].second;
    state.settings.inputMode = InputMode::Stereo;
    std::vector<uint8_t> blob;
    EncodeState(state, blob);
    Check(SetComponentState(*host, blob), "state restored through IComponent::setState");

    WrapperState back{};
    const auto   echoed = ComponentState(*host);
    bool         exact  = DecodeState(echoed.data(), echoed.size(), back);
    for (size_t i = 0; exact && i < kNumLeafParams; ++i) exact = Bits(back.plain[i]) == Bits(state.plain[i]);
    Check(exact, "every plain value read back bit for bit after the controller's echo");

    host->prepareToPlay(48000.0, 512);
    Check(host->getLatencySamples() == 0, "latency 0");
    Check(host->getTailLengthSeconds() > 3600.0, "tail reported as infinite (" +
                                                     juce::String(host->getTailLengthSeconds()) + " s)");

    const Stereo in = MakeInput(2 * 48000);
    Stereo       io = in;
    const int    pattern[] = {512, 37, 441, 0, 1, 256, 0, 480};
    juce::MidiBuffer midi;
    int              pos = 0;
    for (size_t k = 0; pos < static_cast<int>(io.l.size()); ++k) {
      const int                n        = std::min(pattern[k % 8], static_cast<int>(io.l.size()) - pos);
      float*                   chans[2] = {io.l.data() + pos, io.r.data() + pos};
      juce::AudioBuffer<float> buffer(chans, 2, n);
      midi.clear();
      host->processBlock(buffer, midi);
      pos += n;
    }
    const Stereo ref = RenderReference(all, in);
    std::printf("hosted VST3 %016llx, engine reference %016llx\n",
                static_cast<unsigned long long>(Hash(io)), static_cast<unsigned long long>(Hash(ref)));
    Check(SameBits(io.l, ref.l) && SameBits(io.r, ref.r),
          "hosted output equals the 48-frame engine reference (first difference L " +
              juce::String(static_cast<juce::int64>(FirstDiff(io.l, ref.l))) + ")");
    host->releaseResources();
    host.reset();

    // An offline export with "Restart on transport start" on, as a DAW drives one: the
    // transport plays throughout, real time first, then the host switches to offline and
    // the VST3 wrapper passes the mode with every block (setNonRealtime per block). The
    // switch is the one transport start, so the export equals the reference from there.
    struct Playing final : juce::AudioPlayHead {
      juce::Optional<PositionInfo> getPosition() const override {
        PositionInfo info;
        info.setIsPlaying(true);
        return info;
      }
    } playing;
    host = formats.createPluginInstance(*types[0], 48000.0, 512, error);
    Check(host != nullptr && host->setBusesLayout(layout), "a second instance, stereo");
    if (host == nullptr) return 1;
    state.settings.restartOnStart = true;
    EncodeState(state, blob);
    Check(SetComponentState(*host, blob), "restart on transport start restored on");
    host->setPlayHead(&playing);
    host->prepareToPlay(48000.0, 512);
    const auto render = [&](Stereo& s) {
      for (size_t p = 0; p < s.l.size();) {
        const size_t             n        = std::min<size_t>(512, s.l.size() - p);
        float*                   chans[2] = {s.l.data() + p, s.r.data() + p};
        juce::AudioBuffer<float> buffer(chans, 2, static_cast<int>(n));
        midi.clear();
        host->processBlock(buffer, midi);
        p += n;
      }
    };
    Stereo preroll = MakeInput(30011);
    render(preroll);
    host->setNonRealtime(true);
    Stereo exported = in;
    render(exported);
    Check(SameBits(exported.l, ref.l) && SameBits(exported.r, ref.r),
          "an offline export restarts once, at its start: equals the reference (first difference L " +
              juce::String(static_cast<juce::int64>(FirstDiff(exported.l, ref.l))) + ")");
    host->setPlayHead(nullptr);
    host->releaseResources();
  }
  juce::DeletedAtShutdown::deleteAll();
  juce::MessageManager::deleteInstance();
  std::printf("%s\n", gFailures == 0 ? "all checks passed" : "CHECKS FAILED");
  return gFailures == 0 ? 0 : 1;
}
