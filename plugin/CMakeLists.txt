# Desktop plugin and companion app (docs/design/companion-app.md §2-§5). JUCE is used
# under the AGPLv3 (companion §3.5): every target that links it lives under plugin/.

set(BRAINSCAPE_PLUGIN_FORMATS VST3 Standalone)
if(APPLE)
  list(APPEND BRAINSCAPE_PLUGIN_FORMATS AU)  # Logic hosts neither VST3 nor CLAP
endif()

juce_add_plugin(Brainscape
  PRODUCT_NAME                  "Brainscape"
  VERSION                       0.1.0
  COMPANY_NAME                  "Brainscape"
  BUNDLE_ID                     "org.brainscape.Brainscape"
  PLUGIN_MANUFACTURER_CODE      Brsc
  PLUGIN_CODE                   Bscp
  FORMATS                       ${BRAINSCAPE_PLUGIN_FORMATS}
  IS_SYNTH                      FALSE
  NEEDS_MIDI_INPUT              TRUE   # note-on fires a grain
  NEEDS_MIDI_OUTPUT             FALSE
  IS_MIDI_EFFECT                FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS   FALSE
  COPY_PLUGIN_AFTER_BUILD       FALSE  # never write into system plugin folders
  VST3_CATEGORIES               Fx Delay
  MICROPHONE_PERMISSION_ENABLED TRUE
  MICROPHONE_PERMISSION_TEXT    "Brainscape processes your instrument's input."
  HARDENED_RUNTIME_ENABLED      TRUE
  HARDENED_RUNTIME_OPTIONS      com.apple.security.device.audio-input
)

set(BRAINSCAPE_PLUGIN_SOURCES
  src/Audition.cpp
  src/BrainscapeParam.cpp
  src/Curation.cpp
  src/PlainAttachment.cpp
  src/PluginEditor.cpp
  src/PluginProcessor.cpp
  src/StandaloneApp.cpp
  src/StateCodec.cpp
  src/TestInput.cpp
  src/gui/BrainscapeLookAndFeel.cpp
  src/gui/CurationViews.cpp
  src/gui/Widgets.cpp
)
target_sources(Brainscape PRIVATE ${BRAINSCAPE_PLUGIN_SOURCES})
target_include_directories(Brainscape PRIVATE src)
# Warnings are errors on Brainscape's own sources (on in CI); JUCE's module translation
# units, compiled in the same target, keep their warnings as warnings.
if(BRAINSCAPE_WERROR)
  set_source_files_properties(${BRAINSCAPE_PLUGIN_SOURCES} PROPERTIES
    COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/WX,-Werror>")
endif()

# PUBLIC, so every format wrapper (each compiled in its own target that sees only this
# target's interface, JUCEUtils.cmake:1480-1497) is built with the same settings; the
# custom standalone flag would otherwise yield a duplicate juce_CreateApplication.
target_compile_definitions(Brainscape PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0
  # No MIDI CC mapping exists yet; without this, VST3 hosts list 2,080 CC parameters.
  JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=0
  JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1
  $<$<PLATFORM_ID:Windows>:JUCE_ASIO=1>  # the ASIO SDK JUCE bundles is GPLv3 (companion §3.5)
)

# brainscape_dsp PUBLIC for the same reason (companion §3.3): the engine's floating-point
# profile is a PUBLIC usage requirement of brainscape_dsp and must reach every
# translation unit that includes engine headers, the format wrappers included. No LTO on
# anything that links the engine (determinism profile §3.4): JUCE's recommended LTO flags
# are deliberately not linked.
# Exact float comparisons are the parameter layer's point (companion §5.3: plain values
# compare with ==), so -Wfloat-equal is turned back off after JUCE's warning set.
add_library(brainscape_plugin_warnings INTERFACE)
target_compile_options(brainscape_plugin_warnings INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-float-equal>)
target_link_libraries(Brainscape
  PUBLIC
    brainscape_dsp
    # The offline render, its hashes and recipes (tools/audition, mode-compiler.md §8.1, §9.1):
    # JUCE-free, shared with `bspc render`. PUBLIC, as Audition.h includes its header.
    brainscape_audition
  PRIVATE
    brainscape_compiler      # the exact number reader behind typed text (mode-compiler.md §6.5)
    juce::juce_audio_utils
    juce::juce_recommended_warning_flags
    brainscape_plugin_warnings
)
if(MSVC)
  target_compile_options(Brainscape PRIVATE /MP)
endif()

if(BRAINSCAPE_BUILD_TESTS)
  # Headless wrapper checks (companion §4.12) against the shared-code library: the same
  # objects every format wrapper links. The format wrappers re-export the shared code's
  # private include directories in the same way.
  add_executable(brainscape_plugin_tests tests/plugin_tests.cpp)
  # The compiler compiles the test documents (compiler/tests/data) the mode tests load.
  target_link_libraries(brainscape_plugin_tests PRIVATE Brainscape brainscape_compiler)
  target_compile_definitions(brainscape_plugin_tests PRIVATE
    BRAINSCAPE_TEST_DATA="${PROJECT_SOURCE_DIR}/compiler/tests/data"
    BRAINSCAPE_GOLDEN_PRESETS="${PROJECT_SOURCE_DIR}/dsp/tests/golden/presets")
  target_include_directories(brainscape_plugin_tests PRIVATE
    src
    ${PROJECT_SOURCE_DIR}/dsp/tests  # golden/Sha256.h: an independent check of audition hashes
    ${PROJECT_SOURCE_DIR}/dsp/tests/vendor
    $<TARGET_PROPERTY:Brainscape,INCLUDE_DIRECTORIES>)
  if(MSVC)
    target_compile_options(brainscape_plugin_tests PRIVATE /EHsc /bigobj)
  endif()
  add_test(NAME plugin_wrapper COMMAND brainscape_plugin_tests)

  # Renders the editor offscreen to PNG at the default and minimum sizes.
  add_executable(brainscape_editor_snapshot tests/editor_snapshot.cpp)
  target_link_libraries(brainscape_editor_snapshot PRIVATE Brainscape brainscape_compiler)
  target_compile_definitions(brainscape_editor_snapshot PRIVATE
    BRAINSCAPE_TEST_DATA="${PROJECT_SOURCE_DIR}/compiler/tests/data")
  target_include_directories(brainscape_editor_snapshot PRIVATE
    src
    $<TARGET_PROPERTY:Brainscape,INCLUDE_DIRECTORIES>)
  add_test(NAME plugin_editor_snapshot
           COMMAND brainscape_editor_snapshot ${CMAKE_CURRENT_BINARY_DIR}/screenshots)

  # Loads the built VST3 binary through JUCE's headless VST3 host: state in and out,
  # in-place audio against the engine reference. It links JUCE, so it lives here.
  juce_add_console_app(brainscape_vst3_host_check VERSION 0.1.0)
  target_sources(brainscape_vst3_host_check PRIVATE tests/vst3_host_check.cpp src/StateCodec.cpp)
  target_include_directories(brainscape_vst3_host_check PRIVATE src)
  target_compile_definitions(brainscape_vst3_host_check PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0)
  target_link_libraries(brainscape_vst3_host_check PRIVATE
    brainscape_dsp
    juce::juce_audio_processors_headless)
  add_dependencies(brainscape_vst3_host_check Brainscape_VST3)
  add_test(NAME plugin_vst3_hosted
           COMMAND brainscape_vst3_host_check
                   "$<GENEX_EVAL:$<TARGET_PROPERTY:Brainscape_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>")
endif()
