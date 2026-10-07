// Image A, PARITY (firmware/README.md): the golden corpus rendered on the Daisy Seed exactly
// as the golden harness renders it (dsp/tests/golden: the same vectors, presets and event
// scripts, 48-frame blocks, one engine restarted with LoadPreset(..., Exact) for every
// render, the integer test signal as input), offline in the main loop, never in the audio
// callback. Each preset's SHA-256, per-second hashes, counters and DWT cycles stream over
// USB serial as JSON lines (dsp/tests/golden/ParityStream.h); tools/hil/parity_check.py
// compares them with golden.json (determinism-profile.md §6.6, §8.4 step 13). Since sound
// revision 2 the corpus takes its structure from committed packages, which the image carries
// compiled in (dsp/tests/golden/EmbeddedPackages.h): the stream lists each as decoded, for
// parity_check.py to compare with presets/MANIFEST.
//
// Commands, one per line:
//   info                     the hello line (build, board, FP registers, memory map)
//   run [pedal] [hostile] [quick] [only=VECTOR[/PRESET],...]
//                            stream the corpus: the harness's configuration (maxBlockSize
//                            512), or the live engine's (pedal: maxBlockSize 48); hostile
//                            renders with FZ|DN and round toward zero in the caller's FPSCR
//   dfu                      reboot into the bootloader, to flash another image
#include <string>
#include <vector>

#include "EmbeddedPackages.h"
#include "ParityStream.h"
#include "platform/Placement.h"
#include "platform/Platform.h"
#include "platform/UsbSerial.h"

using namespace brainscape;
using namespace brainscape::golden;
using brainscape::fw::Serial;
using brainscape::fw::UsbSerial;

extern "C" uint32_t BrainscapeHeapUsed(void);  // platform/Syscalls.c

namespace {

// Each line goes out whole before the next render starts, so the host sees every preset as
// soon as it is done (and a long render cannot leave a line half sent).
class UsbSink final : public LineSink {
 public:
  void Line(const std::string& json) override {
    Serial().WriteLine(json, UsbSerial::Mode::Block);
    Serial().Flush(2000);
  }
};

std::vector<std::string> Words(const std::string& line) {
  std::vector<std::string> out;
  std::string              cur;
  for (const char c : line) {
    if (c == ' ' || c == '\t') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::vector<std::string> SplitCommas(const std::string& s) {
  std::vector<std::string> out;
  std::string              cur;
  for (const char c : s) {
    if (c == ',') {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

std::string Hello() {
  size_t packages = 0;
  EmbeddedPackages(&packages);
  return fw::HelloJson("parity", "\"target\":" + JsonString(BRAINSCAPE_FW_IMAGE) +
                                     ",\"engineCode\":" + JsonString(BRAINSCAPE_FW_ENGINE_CODE) +
                                     ",\"packages\":" + JsonUInt(packages) +
                                     ",\"commands\":[\"info\",\"run [pedal] [hostile] [quick] "
                                     "[only=...]\",\"dfu\"]");
}

void Error(const std::string& message) {
  Serial().WriteLine(JsonObj().Str("type", "error").Str("message", message).Done(),
                     UsbSerial::Mode::Block);
}

void Run(const std::vector<std::string>& words) {
  RenderConfig  rc;  // 48-frame blocks, the 2^22 ring, engine events, a restarted engine
  StreamOptions so;
  so.tag = "daisy-seed";
  for (size_t i = 1; i < words.size(); ++i) {
    const std::string& w = words[i];
    if (w == "pedal") {
      rc.maxBlockSize = 48;
    } else if (w == "hostile") {
      rc.fpEnv = FpEnv::Hostile;
    } else if (w == "quick") {
      so.quick = true;
    } else if (w.rfind("only=", 0) == 0) {
      so.only = SplitCommas(w.substr(5));
    } else {
      Error("unknown run option: " + w);
      return;
    }
  }
  const fw::EnginePlacement placement = fw::Placement();
  EngineConfig              ec;
  ec.maxBlockSize = rc.maxBlockSize;
  const char* why = nullptr;
  if (!fw::CheckPlacement(ec, placement, &why)) {
    Error(why);
    return;
  }
  Renderer::Placement rp;
  rp.arenas = placement.arenas;
  rp.engine = placement.engine;
  Renderer renderer(rc, rp);
  if (!renderer.ok()) {
    Error("the renderer refused the placement");
    return;
  }
  so.clock        = fw::Cycles64;
  so.clockHz      = fw::SysClkHz();
  so.clockName    = "dwt";
  so.platformJson = JsonObj()
                        .Str("name", "daisy-seed")
                        .Str("board", fw::BoardVersionName())
                        .Str("appType", fw::Build().appType)
                        .Str("engineCode", BRAINSCAPE_FW_ENGINE_CODE)
                        .Str("engineArchiveSha256", fw::Build().engineArchiveSha256)
                        .Str("firmwareCommit", fw::Build().commit)
                        .Raw("arenas", "\"Hot+Engine: DTCM, Warm: AXI SRAM, Bulk: SDRAM\"")
                        .Done();
  fw::SetLedMode(fw::LedMode::Busy);
  UsbSink            sink;
  const StreamResult r = StreamCorpus(renderer, so, sink);
  fw::SetLedMode(r.renderFailures == 0 ? fw::LedMode::Done : fw::LedMode::Fault);
  // After parity-end: whether the transport lost anything (parity_check.py reads it and
  // fails the run on any loss).
  Serial().WriteLine(JsonObj()
                         .Str("type", "idle")
                         .Hex("fpscr", fw::ReadFpscr())
                         .UInt("heapUsed", BrainscapeHeapUsed())
                         .UInt("droppedBytes", Serial().DroppedBytes())
                         .UInt("droppedLines", Serial().DroppedLines())
                         .Done(),
                     UsbSerial::Mode::Block);
  Serial().Flush(2000);
}

}  // namespace

int main() {
  fw::BoardInit("parity");
  const fw::EnginePlacement placement = fw::Placement();
  EngineConfig              ec;  // the largest configuration the image renders
  const char*               why = nullptr;
  if (!fw::CheckPlacement(ec, placement, &why)) fw::Fatal(why);

  Serial().WriteLine(Hello(), UsbSerial::Mode::Drop);
  std::string line;
  for (;;) {
    Serial().Pump();
    if (!Serial().ReadLine(&line)) continue;
    const std::vector<std::string> words = Words(line);
    if (words.empty()) continue;
    if (words[0] == "info") {
      Serial().WriteLine(Hello(), UsbSerial::Mode::Block);
    } else if (words[0] == "run") {
      Run(words);
    } else if (words[0] == "dfu") {
      fw::RebootToBootloader();
    } else {
      Error("unknown command: " + words[0]);
    }
  }
}
