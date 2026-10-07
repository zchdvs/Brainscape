// bspc, the preset compiler's command-line tool (docs/design/mode-compiler.md §8.2). Every
// command is a thin shell over brainscape_compiler; the files it writes are pure functions of
// their inputs and this build's constants (§8.3).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#include "Compile.h"
#include "Lint.h"
#include "Migrate.h"
#include "Text.h"
#include "brainscape/SoundRevision.h"

namespace {

using bsc::Bytes;
using bsc::Finding;

void Out(std::string_view s) { std::fwrite(s.data(), 1, s.size(), stdout); }
void Err(std::string_view s) { std::fwrite(s.data(), 1, s.size(), stderr); }
void ErrLine(const std::string& s) { Err(s + "\n"); }

constexpr int kOk = 0, kFound = 1, kUsage = 2;

bool ReadFile(const std::string& path, Bytes* out) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return false;
  out->clear();
  uint8_t buf[65536];
  size_t  n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out->insert(out->end(), buf, buf + n);
  const bool ok = std::ferror(f) == 0;
  std::fclose(f);
  return ok;
}

bool ReadText(const std::string& path, std::string* out) {
  Bytes b;
  if (!ReadFile(path, &b)) return false;
  out->assign(reinterpret_cast<const char*>(b.data()), b.size());
  return true;
}

bool WriteFile(const std::string& path, const void* data, size_t length) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return false;
  const bool ok = std::fwrite(data, 1, length, f) == length;
  return std::fclose(f) == 0 && ok;
}

std::string Normalize(std::string path) {
  std::replace(path.begin(), path.end(), '\\', '/');
  return path;
}

std::string WithExtension(const std::string& path, const char* ext) {
  const size_t slash = path.find_last_of("/\\");
  const size_t dot   = path.find_last_of('.');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
    return path.substr(0, dot) + ext;
  }
  return path + ext;
}

bool EndsWith(const std::string& s, const char* suffix) {
  const size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Prints findings; true when one is an error.
bool Report(const std::vector<Finding>& findings, const std::string& file) {
  bool error = false;
  for (const Finding& f : findings) {
    ErrLine(bsc::Describe(f, file));
    error = error || f.error;
  }
  return error;
}

struct Args {
  std::vector<std::string> files;
  std::vector<std::string> flags;
  std::string              output;
  std::string              expect;
  std::string              manifestOut;
  std::string              id, name;
  bool                     Has(const char* flag) const {
                        return std::find(flags.begin(), flags.end(), flag) != flags.end();
  }
};

bool Parse(int argc, char** argv, Args* a) {
  for (int i = 2; i < argc; ++i) {
    const std::string s     = argv[i];
    const auto        value = [&](std::string* out) {
      if (i + 1 >= argc) return false;
      *out = argv[++i];
      return true;
    };
    if (s == "-o") {
      if (!value(&a->output)) return false;
    } else if (s == "--expect") {
      if (!value(&a->expect)) return false;
    } else if (s == "--write-manifest") {
      if (!value(&a->manifestOut)) return false;
    } else if (s == "--id") {
      if (!value(&a->id)) return false;
    } else if (s == "--name") {
      if (!value(&a->name)) return false;
    } else if (s.size() > 2 && s.compare(0, 2, "--") == 0) {
      a->flags.push_back(s);
    } else {
      a->files.push_back(s);
    }
  }
  return true;
}

int Usage() {
  Err("usage: bspc <command> [options] files\n"
      "  compile DOC.json [-o OUT.bsp]      compile a preset document to a package (default: "
      "DOC.bsp)\n"
      "  decompile PKG.bsp [--rebuild] [-o OUT.json]\n"
      "                                     the JSON section, or the document rebuilt from the\n"
      "                                     package when it has none or it is stale\n"
      "  fmt [--check] DOC.json...          rewrite in canonical form; --check lists the files\n"
      "                                     that differ\n"
      "  verify PKG.bsp...                  hashes, structure, and that the JSON section compiles\n"
      "                                     to the same STAT and MODE\n"
      "  stamp [--check] FILE...            documents: rewrite sound_rev and sound_hash for this\n"
      "                                     build; packages: recompile from their JSON section\n"
      "  lint [--factory] DOC.json...       lint findings L1-L9 (--factory: L4, L7-L9 are errors)\n"
      "  diff A.bsp B.bsp                   the first differing field, by name\n"
      "  derive [--solve] DOC.json...       targeted leaves from macro positions (--solve:\n"
      "                                     positions from leaves), rewritten in place\n"
      "  roundtrip [--expect M] [--write-manifest M] DOC.json...\n"
      "                                     the bspc-roundtrip checks (§8.3, §10.1) and a sorted\n"
      "                                     manifest of package, sound and control hashes\n"
      "  migrate-session SESSION [--id ID] [--name NAME] [-o OUT.json]\n"
      "                                     a BSWS v1 plugin session as a preset document\n"
      "  version                            this build's sound revision and formats\n");
  return kUsage;
}

int CmdCompile(const Args& a) {
  if (a.files.size() != 1u) return Usage();
  std::string text;
  if (!ReadText(a.files[0], &text)) {
    ErrLine("bspc: cannot read " + a.files[0]);
    return kUsage;
  }
  const bsc::CompileResult r = bsc::Compile(text);
  if (Report(r.findings, a.files[0]) || !r.ok) return kFound;
  const std::string out = a.output.empty() ? WithExtension(a.files[0], ".bsp") : a.output;
  if (!WriteFile(out, r.package.data(), r.package.size())) {
    ErrLine("bspc: cannot write " + out);
    return kUsage;
  }
  ErrLine(out + ": " + bsc::Dec(r.package.size()) + " bytes, sound_hash " +
          bsc::Hex(r.soundHash.bytes, 32));
  return kOk;
}

int CmdDecompile(const Args& a) {
  if (a.files.size() != 1u) return Usage();
  Bytes b;
  if (!ReadFile(a.files[0], &b)) {
    ErrLine("bspc: cannot read " + a.files[0]);
    return kUsage;
  }
  const bsc::DecompileResult r = bsc::Decompile(b.data(), b.size(), a.Has("--rebuild"));
  if (Report(r.findings, a.files[0]) || !r.ok) return kFound;
  if (r.rebuilt)
    ErrLine(a.files[0] + ": rebuilt from STAT, MODE, CTRL and META (editor data lost)");
  if (a.output.empty()) {
    Out(r.json);
  } else if (!WriteFile(a.output, r.json.data(), r.json.size())) {
    ErrLine("bspc: cannot write " + a.output);
    return kUsage;
  }
  return kOk;
}

int CmdFmt(const Args& a) {
  if (a.files.empty()) return Usage();
  const bool check = a.Has("--check");
  int        rc    = kOk;
  for (const std::string& file : a.files) {
    std::string text, canonical;
    if (!ReadText(file, &text)) {
      ErrLine("bspc: cannot read " + file);
      return kUsage;
    }
    std::vector<Finding> findings;
    if (!bsc::FormatText(text, &canonical, &findings)) {
      Report(findings, file);
      rc = kFound;
      continue;
    }
    if (canonical == text) continue;
    if (check) {
      Out(Normalize(file) + "\n");
      rc = kFound;
    } else if (!WriteFile(file, canonical.data(), canonical.size())) {
      ErrLine("bspc: cannot write " + file);
      return kUsage;
    } else {
      ErrLine(file + ": formatted");
    }
  }
  return rc;
}

int CmdVerify(const Args& a) {
  if (a.files.empty()) return Usage();
  int rc = kOk;
  for (const std::string& file : a.files) {
    Bytes b;
    if (!ReadFile(file, &b)) {
      ErrLine("bspc: cannot read " + file);
      return kUsage;
    }
    const std::vector<Finding> findings = bsc::Verify(b.data(), b.size());
    if (Report(findings, file)) {
      rc = kFound;
    } else {
      ErrLine(file + ": verified");
    }
  }
  return rc;
}

int CmdStamp(const Args& a) {
  if (a.files.empty()) return Usage();
  const bool check = a.Has("--check");
  int        rc    = kOk;
  for (const std::string& file : a.files) {
    Bytes b;
    if (!ReadFile(file, &b)) {
      ErrLine("bspc: cannot read " + file);
      return kUsage;
    }
    if (EndsWith(file, ".bsp")) {
      const bsc::DecodedPackage p = bsc::DecodePackage(b.data(), b.size());
      if (!p.ok || !p.hasJson || (p.info.flags & brainscape::kPackageFlagJsonStale) != 0u) {
        ErrLine(file +
                ": no current JSON section to recompile (decompile --rebuild, then compile)");
        rc = kFound;
        continue;
      }
      const bsc::CompileResult r = bsc::Compile(p.json);
      if (Report(r.findings, file) || !r.ok) {
        rc = kFound;
        continue;
      }
      if (r.package == b) continue;
      if (check) {
        Out(Normalize(file) + "\n");
        rc = kFound;
      } else if (!WriteFile(file, r.package.data(), r.package.size())) {
        ErrLine("bspc: cannot write " + file);
        return kUsage;
      } else {
        ErrLine(file + ": stamped");
      }
      continue;
    }
    const std::string        text(reinterpret_cast<const char*>(b.data()), b.size());
    const bsc::CompileResult r = bsc::Compile(text);
    if (Report(r.findings, file) || !r.ok) {
      rc = kFound;
      continue;
    }
    if (r.json == text) continue;
    if (check) {
      Out(Normalize(file) + "\n");
      rc = kFound;
    } else if (!WriteFile(file, r.json.data(), r.json.size())) {
      ErrLine("bspc: cannot write " + file);
      return kUsage;
    } else {
      ErrLine(file + ": stamped (sound_rev " + bsc::Dec(brainscape::kSoundRevision) + ")");
    }
  }
  return rc;
}

int CmdLint(const Args& a) {
  if (a.files.empty()) return Usage();
  bsc::LintOptions options;
  options.factory = a.Has("--factory");
  int rc          = kOk;
  for (const std::string& file : a.files) {
    std::string text;
    if (!ReadText(file, &text)) {
      ErrLine("bspc: cannot read " + file);
      return kUsage;
    }
    std::vector<Finding> findings;
    bsc::Document        doc;
    if (!bsc::ReadDocumentText(text, bsc::ReadOptions{}, &doc, &findings)) {
      Report(findings, file);
      rc = kFound;
      continue;
    }
    if (Report(bsc::Lint(doc, options), file)) rc = kFound;
  }
  return rc;
}

int CmdDiff(const Args& a) {
  if (a.files.size() != 2u) return Usage();
  Bytes x, y;
  if (!ReadFile(a.files[0], &x) || !ReadFile(a.files[1], &y)) {
    ErrLine("bspc: cannot read the packages");
    return kUsage;
  }
  const std::string d = bsc::Diff(x.data(), x.size(), y.data(), y.size());
  if (d.empty()) return kOk;
  Out(d + "\n");
  return kFound;
}

int CmdDerive(const Args& a) {
  if (a.files.empty()) return Usage();
  int rc = kOk;
  for (const std::string& file : a.files) {
    std::string text;
    if (!ReadText(file, &text)) {
      ErrLine("bspc: cannot read " + file);
      return kUsage;
    }
    std::vector<Finding> findings;
    bsc::Document        doc;
    if (!bsc::ReadDocumentText(text, bsc::ReadOptions{}, &doc, &findings)) {
      Report(findings, file);
      rc = kFound;
      continue;
    }
    std::vector<std::string> log;
    bsc::Derive(&doc, a.Has("--solve"), &log);
    for (const std::string& line : log) ErrLine(file + ": " + line);
    const std::string out = bsc::FormatDocument(doc);
    if (out != text && !WriteFile(file, out.data(), out.size())) {
      ErrLine("bspc: cannot write " + file);
      return kUsage;
    }
  }
  return rc;
}

// The bspc-roundtrip checks of one document (§8.3, §10.1). Returns the manifest line, or "".
std::string RoundTrip(const std::string& file, bool* ok) {
  const auto fail = [&](const std::string& why) {
    ErrLine(file + ": " + why);
    *ok = false;
  };
  std::string text;
  if (!ReadText(file, &text)) {
    fail("cannot read");
    return std::string();
  }
  const bsc::CompileResult r = bsc::Compile(text);
  if (Report(r.findings, file) || !r.ok) {
    fail("does not compile");
    return std::string();
  }
  // The committed package, when there is one, byte for byte.
  Bytes committed;
  if (ReadFile(WithExtension(file, ".bsp"), &committed) && committed != r.package) {
    fail("compiles to other bytes than " + WithExtension(file, ".bsp"));
  }
  // fmt --check: the document is canonical, and Fmt is idempotent.
  std::string          fmt, fmt2;
  std::vector<Finding> findings;
  if (!bsc::FormatText(text, &fmt, &findings) || !bsc::FormatText(fmt, &fmt2, &findings)) {
    fail("does not format");
  } else {
    if (fmt != text) fail("is not in canonical form (bspc fmt)");
    if (fmt2 != fmt) fail("formatting is not idempotent");
  }
  // Decompile(Compile(J)) = Fmt(J): the JSON section, and a stamp that is current.
  const bsc::DecompileResult d = bsc::Decompile(r.package.data(), r.package.size());
  if (!d.ok || d.json != fmt) fail("Decompile(Compile(J)) differs from Fmt(J): bspc stamp?");
  // Without the JSON section: Fmt(J) minus editor data.
  const bsc::DecompileResult rb = bsc::Decompile(r.package.data(), r.package.size(), true);
  if (!rb.ok || rb.json != bsc::FormatDocument(r.doc, false)) {
    fail("the package rebuilt without its JSON section differs from Fmt(J) minus editor data");
  }
  // The JSON section compiles to the same bytes.
  const bsc::CompileResult again = bsc::Compile(r.json);
  if (!again.ok || again.package != r.package)
    fail("the JSON section does not recompile to the same bytes");
  return bsc::Hex(r.packageHash.bytes, 32) + " " + bsc::Hex(r.soundHash.bytes, 32) + " " +
         bsc::Hex(r.controlHash.bytes, 32) + " " + Normalize(file) + "\n";
}

int CmdRoundTrip(const Args& a) {
  if (a.files.empty()) return Usage();
  std::vector<std::string> files = a.files;
  std::vector<std::string> lines;
  bool                     ok = true;
  for (const std::string& file : files) {
    const std::string line = RoundTrip(file, &ok);
    if (!line.empty()) lines.push_back(line);
  }
  std::sort(lines.begin(), lines.end(), [](const std::string& x, const std::string& y) {
    return x.substr(195) < y.substr(195);  // by path, after three hashes and spaces
  });
  std::string manifest;
  for (const std::string& line : lines) manifest += line;
  Out(manifest);
  if (!a.manifestOut.empty() && !WriteFile(a.manifestOut, manifest.data(), manifest.size())) {
    ErrLine("bspc: cannot write " + a.manifestOut);
    return kUsage;
  }
  if (!a.expect.empty()) {
    std::string expected;
    if (!ReadText(a.expect, &expected)) {
      ErrLine("bspc: cannot read " + a.expect);
      return kUsage;
    }
    if (expected != manifest) {
      ErrLine("bspc: the manifest differs from " + a.expect);
      ok = false;
    }
  }
  if (ok) ErrLine("bspc roundtrip: " + bsc::Dec(lines.size()) + " documents, all checks passed");
  return ok ? kOk : kFound;
}

int CmdMigrateSession(const Args& a) {
  if (a.files.size() != 1u) return Usage();
  Bytes b;
  if (!ReadFile(a.files[0], &b)) {
    ErrLine("bspc: cannot read " + a.files[0]);
    return kUsage;
  }
  std::vector<Finding> findings;
  bsc::Document        doc;
  const bool ok = bsc::MigrateSession(b.data(), b.size(), a.id.empty() ? "user.session" : a.id,
                                      a.name.empty() ? "Session" : a.name, &doc, &findings);
  Report(findings, a.files[0]);
  if (!ok) return kFound;
  const std::string text = bsc::FormatDocument(doc);
  if (a.output.empty()) {
    Out(text);
  } else if (!WriteFile(a.output, text.data(), text.size())) {
    ErrLine("bspc: cannot write " + a.output);
    return kUsage;
  }
  return kOk;
}

int CmdVersion() {
  Out("bspc: sound revision " + bsc::Dec(brainscape::kSoundRevision) + ", package format " +
      bsc::Dec(brainscape::kPackageFormat) + ", blob format " + bsc::Dec(brainscape::kBlobFormat) +
      ", schema " + bsc::Dec(brainscape::kSchemaVersion) + ", mode features " +
      bsc::Dec(brainscape::kSupportedModeFeatures) + "\n");
  return kOk;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  // Bytes out as written: no CR before LF on Windows.
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  if (argc < 2) return Usage();
  const std::string cmd = argv[1];
  Args              a;
  if (!Parse(argc, argv, &a)) return Usage();
  if (cmd == "compile") return CmdCompile(a);
  if (cmd == "decompile") return CmdDecompile(a);
  if (cmd == "fmt") return CmdFmt(a);
  if (cmd == "verify") return CmdVerify(a);
  if (cmd == "stamp") return CmdStamp(a);
  if (cmd == "lint") return CmdLint(a);
  if (cmd == "diff") return CmdDiff(a);
  if (cmd == "derive") return CmdDerive(a);
  if (cmd == "roundtrip") return CmdRoundTrip(a);
  if (cmd == "migrate-session") return CmdMigrateSession(a);
  if (cmd == "version") return CmdVersion();
  return Usage();
}
