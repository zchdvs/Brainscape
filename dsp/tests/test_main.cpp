#define CATCH_CONFIG_RUNNER
#include "catch.hpp"

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <stdlib.h>
#endif

int main(int argc, char* argv[]) {
#if defined(_MSC_VER)
  // MSVC's debug CRT answers a failed assert or abort() with a dialog, which hangs an
  // unattended run (CI, ctest); report on stderr and exit instead.
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  for (int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
    _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
  }
#endif
  return Catch::Session().run(argc, argv);
}
