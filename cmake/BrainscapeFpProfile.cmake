# Floating-point build profile (docs/design/determinism-profile.md §3.2-§3.6).
#
# brainscape::fp_profile carries the flags that make every conforming build produce
# bit-identical engine output: contraction off, no fast-math, no errno fallbacks behind
# square roots. brainscape_dsp links it PUBLIC, so every translation unit that compiles
# or inlines engine code (tests, firmware, the plugin's format wrappers) gets it too.
include_guard(GLOBAL)

# No 32-bit x86 (§3.6): x87 evaluates in 80 bits, and nothing tests SSE-only 32-bit builds.
if(CMAKE_SIZEOF_VOID_P EQUAL 4 AND
   (CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "^(X86|x86|i[3-6]86)$" OR
    CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86|X86|i[3-6]86|AMD64|amd64|x86_64)$"))
  message(FATAL_ERROR "brainscape determinism profile: 32-bit x86 targets are not supported "
                      "(determinism-profile.md §3.6)")
endif()

add_library(brainscape_fp_profile INTERFACE)
add_library(brainscape::fp_profile ALIAS brainscape_fp_profile)

# TEST-ONLY escape for the negative control (§6.4): a build with contraction ON must NOT
# reproduce the reference hashes, or the golden corpus cannot see contraction. It swaps
# the profile's contraction flag for its opposite, lets the configure check below accept
# contraction (and nothing else it forbids), skips the private headers' FP_CONTRACT
# pragmas, and defines BRAINSCAPE_FP_NEGATIVE_CONTROL for every consumer: the golden
# harness then refuses check and mint and only reports, and the render tool says so.
# Never for anything shipped, so it is test-only by construction:
#  - it is read from -DBRAINSCAPE_FP_NEGATIVE_CONTROL=ON and never cached, so it lasts
#    one configure: a build directory reconfigured without the -D conforms again;
#  - it refuses plugin and firmware builds, and the plugin's sources (and the firmware's,
#    when they land) refuse the macro with #error, so a hand-defined one fails too;
#  - brainscape_dsp prints it at build time (dsp/CMakeLists.txt).
if(BRAINSCAPE_FP_NEGATIVE_CONTROL)
  set(BRAINSCAPE_FP_NEGATIVE_CONTROL ON)
else()
  set(BRAINSCAPE_FP_NEGATIVE_CONTROL OFF)
endif()
unset(BRAINSCAPE_FP_NEGATIVE_CONTROL CACHE)
if(BRAINSCAPE_FP_NEGATIVE_CONTROL AND (BRAINSCAPE_BUILD_PLUGIN OR BRAINSCAPE_BUILD_FIRMWARE))
  message(FATAL_ERROR "brainscape determinism profile: BRAINSCAPE_FP_NEGATIVE_CONTROL is "
                      "test-only and cannot build the plugin or the firmware; configure the "
                      "negative control in its own build directory with BRAINSCAPE_BUILD_PLUGIN "
                      "and BRAINSCAPE_BUILD_FIRMWARE off")
endif()
if(BRAINSCAPE_FP_NEGATIVE_CONTROL)
  set(_bs_contract_gnu -ffp-contract=fast)
  set(_bs_contract_msvc /fp:contract)
  target_compile_definitions(brainscape_fp_profile INTERFACE BRAINSCAPE_FP_NEGATIVE_CONTROL=1)
  message(WARNING
    "\n"
    "  ********************************************************************\n"
    "  *  BRAINSCAPE_FP_NEGATIVE_CONTROL=ON: floating-point contraction ON  *\n"
    "  *  This build is NOT profile-conforming (determinism-profile.md     *\n"
    "  *  §3.2). Its output must DIFFER from every conforming build. The   *\n"
    "  *  golden harness only reports; never ship, mint or check with it.  *\n"
    "  *  Not cached: reconfigure without the -D to conform again.         *\n"
    "  ********************************************************************\n")
else()
  set(_bs_contract_gnu -ffp-contract=off)
  set(_bs_contract_msvc "")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  # Before VS 2022 17.0, /fp:precise still contracted on some targets.
  if(MSVC_VERSION LESS 1930)
    message(FATAL_ERROR "brainscape determinism profile: MSVC from Visual Studio 2022 17.0 "
                        "(MSVC_VERSION 1930) is required, found ${MSVC_VERSION}")
  endif()
  target_compile_options(brainscape_fp_profile INTERFACE /fp:precise ${_bs_contract_msvc})
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  # clang-cl: /fp:precise implies -ffp-contract=on, so the override must follow it.
  target_compile_options(brainscape_fp_profile INTERFACE
    /fp:precise /clang:${_bs_contract_gnu} /clang:-fno-math-errno)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
  # Order matters: -fno-fast-math turns math-errno back on (GCC) and contraction to "on"
  # (Clang), so it comes first.
  target_compile_options(brainscape_fp_profile INTERFACE
    -fno-fast-math ${_bs_contract_gnu} -fno-math-errno)
else()
  message(FATAL_ERROR "brainscape determinism profile: no flag set for compiler "
                      "'${CMAKE_CXX_COMPILER_ID}' (determinism-profile.md §3.2)")
endif()

# Flags that break identity (§3.2, §3.6) or inline engine code across the boundary
# (§3.4): one list, fp-forbidden-flags.txt, which tools/ci/audit_flags.py reads too, so
# the configure check and the command-line audit cannot drift. A flag may sit inside a
# generator expression, between commas: JUCE's juce_recommended_lto_flags writes
# $<IF:...,-GL,-flto>. A global property, because the check runs deferred in the
# top-level directory's scope.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/fp-forbidden-flags.txt" _forbidden REGEX "^[^#]")
list(TRANSFORM _forbidden STRIP)
list(REMOVE_ITEM _forbidden "")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_LIST_DIR}/fp-forbidden-flags.txt")
if(NOT _forbidden)
  message(FATAL_ERROR "brainscape determinism profile: no entries read from "
                      "${CMAKE_CURRENT_LIST_DIR}/fp-forbidden-flags.txt")
endif()
if(NOT BRAINSCAPE_FP_NEGATIVE_CONTROL)  # the escape above admits contraction only
  list(APPEND _forbidden "-ffp-contract=(fast|on|fast-honor-pragmas)" "[/-]fp:contract")
endif()
list(JOIN _forbidden "|" _forbidden)
set_property(GLOBAL PROPERTY BRAINSCAPE_FP_FORBIDDEN_RE
             "(^|[ ;:>,\"'])(${_forbidden})($|[ ;>,\"'])")
unset(_forbidden)

function(_brainscape_fp_scan text where outvar)
  get_property(re GLOBAL PROPERTY BRAINSCAPE_FP_FORBIDDEN_RE)
  set(hits "${${outvar}}")
  if(text MATCHES "${re}")
    list(APPEND hits "${where}: '${CMAKE_MATCH_2}'")
  endif()
  set(${outvar} "${hits}" PARENT_SCOPE)
endfunction()

function(_brainscape_fp_all_targets dir outvar)
  get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
  get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
  foreach(sub IN LISTS subdirs)
    _brainscape_fp_all_targets("${sub}" subtargets)
    list(APPEND targets ${subtargets})
  endforeach()
  set(${outvar} "${targets}" PARENT_SCOPE)
endfunction()

# Every target reachable from tgt through its link libraries, aliases resolved.
function(_brainscape_fp_link_closure tgt outvar)
  set(seen "")
  set(queue "${tgt}")
  while(queue)
    list(POP_FRONT queue t)
    get_target_property(aliased "${t}" ALIASED_TARGET)
    if(aliased)
      set(t "${aliased}")
    endif()
    if(t IN_LIST seen)
      continue()
    endif()
    list(APPEND seen "${t}")
    set(items "")
    foreach(prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
      get_target_property(v "${t}" ${prop})
      if(v)
        list(APPEND items ${v})
      endif()
    endforeach()
    foreach(item IN LISTS items)
      string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" item "${item}")
      string(REGEX REPLACE "::@\\(.*\\)$" "" item "${item}")
      if(item AND TARGET "${item}")
        list(APPEND queue "${item}")
      endif()
    endforeach()
  endwhile()
  set(${outvar} "${seen}" PARENT_SCOPE)
endfunction()

# Configure-time check (§3.2, §3.4): no target that compiles or links the engine may
# carry a forbidden flag or interprocedural optimization, from any source: the flag
# variables, arguments that came with the compiler (CXX="g++ -flag" lands in
# CMAKE_CXX_COMPILER_ARG1), compiler launchers, and target and source options.
function(_brainscape_fp_check)
  set(configs DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  foreach(c IN LISTS CMAKE_CONFIGURATION_TYPES CMAKE_BUILD_TYPE)
    string(TOUPPER "${c}" c)
    list(APPEND configs "${c}")
  endforeach()
  list(REMOVE_DUPLICATES configs)
  set(flagvars CMAKE_CXX_FLAGS CMAKE_CXX_COMPILER_ARG1 CMAKE_CXX_COMPILER_LAUNCHER)
  set(ipoprops INTERPROCEDURAL_OPTIMIZATION)
  foreach(c IN LISTS configs)
    list(APPEND flagvars CMAKE_CXX_FLAGS_${c})
    list(APPEND ipoprops INTERPROCEDURAL_OPTIMIZATION_${c})
  endforeach()

  set(hits "")
  _brainscape_fp_all_targets("${CMAKE_SOURCE_DIR}" targets)
  foreach(t IN LISTS targets)
    _brainscape_fp_link_closure("${t}" closure)
    if(NOT "brainscape_dsp" IN_LIST closure)
      continue()
    endif()
    get_target_property(dir "${t}" SOURCE_DIR)
    foreach(var IN LISTS flagvars)
      get_directory_property(v DIRECTORY "${dir}" DEFINITION ${var})
      _brainscape_fp_scan("${v}" "${var} (${dir})" hits)
    endforeach()
    foreach(prop COMPILE_OPTIONS COMPILE_FLAGS CXX_COMPILER_LAUNCHER)
      get_target_property(v "${t}" ${prop})
      if(v)
        _brainscape_fp_scan("${v}" "${t} ${prop}" hits)
      endif()
    endforeach()
    # Per-source options follow the profile flags on the command line, so they win.
    get_target_property(srcs "${t}" SOURCES)
    foreach(src IN LISTS srcs)
      if(src MATCHES "\\$<")
        continue()
      endif()
      if(NOT IS_ABSOLUTE "${src}")
        set(src "${dir}/${src}")
      endif()
      foreach(prop COMPILE_OPTIONS COMPILE_FLAGS)
        get_source_file_property(v "${src}" TARGET_DIRECTORY "${t}" ${prop})
        if(v)
          _brainscape_fp_scan("${v}" "${t} ${src} ${prop}" hits)
        endif()
      endforeach()
    endforeach()
    foreach(dep IN LISTS closure)
      get_target_property(v "${dep}" INTERFACE_COMPILE_OPTIONS)
      if(v)
        _brainscape_fp_scan("${v}" "${dep} INTERFACE_COMPILE_OPTIONS (via ${t})" hits)
      endif()
    endforeach()
    foreach(prop IN LISTS ipoprops)
      get_target_property(v "${t}" ${prop})
      if(v)
        list(APPEND hits "${t} ${prop}: '${v}'")
      endif()
    endforeach()
  endforeach()
  if(hits)
    list(REMOVE_DUPLICATES hits)
    list(JOIN hits "\n  " hits)
    message(FATAL_ERROR "brainscape determinism profile: forbidden floating-point or LTO "
                        "settings on targets that compile or link the engine "
                        "(determinism-profile.md §3.2, §3.4):\n  ${hits}")
  endif()
endfunction()

# Deferred to the end of the top-level directory, after every target and flag is final.
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _brainscape_fp_check)
