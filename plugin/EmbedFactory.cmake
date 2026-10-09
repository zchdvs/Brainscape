# Writes the factory set's committed packages as a C++ table (FactoryModes.h, FactoryPackages):
# every package firmware/factory/MANIFEST lists, in its order, from the .bsp beside each
# document, byte for byte, with the document's id, name and family and whether it is one of
# reserve/'s. Run at build time (plugin/CMakeLists.txt), so the table follows the committed
# files, as dsp/tests/golden/EmbedPackages.cmake does for the golden corpus.
#   cmake -DFACTORY=firmware/factory -DOUT=FactoryPackages.cpp -P EmbedFactory.cmake
# The build fails when a package's header does not carry MANIFEST's three hashes; the plugin
# tests check the hashes against the bytes themselves (DecodePreset and an independent SHA-256).
foreach(v FACTORY OUT)
  if(NOT DEFINED ${v})
    message(FATAL_ERROR "EmbedFactory.cmake: ${v} is not set")
  endif()
endforeach()
if(NOT EXISTS "${FACTORY}/MANIFEST")
  message(FATAL_ERROR "EmbedFactory.cmake: ${FACTORY}/MANIFEST is missing")
endif()

# bspc roundtrip --write-manifest's lines: package, sound and control hashes, then the
# document's path relative to the set (reserve/ for the reserves).
file(STRINGS "${FACTORY}/MANIFEST" lines)
string(REPEAT "0x[0-9a-f][0-9a-f]," 16 byte16)
set(arrays "")
set(table "")
set(index 0)
foreach(line IN LISTS lines)
  if(NOT line MATCHES "^([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+) ((reserve/)?[A-Za-z0-9_.-]+)[.]json$")
    message(FATAL_ERROR "EmbedFactory.cmake: not a manifest line in ${FACTORY}/MANIFEST: '${line}'")
  endif()
  set(packageHash "${CMAKE_MATCH_1}")
  set(soundHash "${CMAKE_MATCH_2}")
  set(controlHash "${CMAKE_MATCH_3}")
  set(stem "${CMAKE_MATCH_4}")
  foreach(h packageHash soundHash controlHash)
    string(LENGTH "${${h}}" n)
    if(NOT n EQUAL 64)
      message(FATAL_ERROR "EmbedFactory.cmake: ${stem}.json's ${h} in MANIFEST is not 64 hex digits")
    endif()
  endforeach()
  if(stem MATCHES "^reserve/")
    set(reserve true)
  else()
    set(reserve false)
  endif()

  # The document's identity: its id, name and family (the package's META holds the same; the
  # tests compare them).
  set(json "${FACTORY}/${stem}.json")
  if(NOT EXISTS "${json}")
    message(FATAL_ERROR "EmbedFactory.cmake: ${json}, which MANIFEST lists, is missing")
  endif()
  file(READ "${json}" document)
  string(JSON id ERROR_VARIABLE err GET "${document}" id)
  if(err OR NOT id MATCHES "^[a-z0-9._-]+$")
    message(FATAL_ERROR "EmbedFactory.cmake: ${json} has no id this table can hold (${err})")
  endif()
  string(JSON name ERROR_VARIABLE err GET "${document}" name)
  if(err OR name STREQUAL "")
    message(FATAL_ERROR "EmbedFactory.cmake: ${json} has no name (${err})")
  endif()
  string(JSON family ERROR_VARIABLE err GET "${document}" family)
  if(family STREQUAL "echoic")
    set(familyEnum Echoic)
  elseif(family STREQUAL "reverie")
    set(familyEnum Reverie)
  elseif(family STREQUAL "recall")
    set(familyEnum Recall)
  elseif(family STREQUAL "misfire")
    set(familyEnum Misfire)
  else()
    message(FATAL_ERROR "EmbedFactory.cmake: ${json}'s family '${family}' is not one of the four")
  endif()
  # The name as a literal of its UTF-8 bytes: letters, digits and spaces as themselves, every
  # other byte escaped and closing its literal ("D\xc3" "\xa9" "j..."), so the source stays
  # ASCII whatever the compiler's character set and no escape runs into the next character.
  string(HEX "${name}" nameHex)
  string(LENGTH "${nameHex}" hexLength)
  set(nameLiteral "\"")
  set(i 0)
  while(i LESS hexLength)
    string(SUBSTRING "${nameHex}" ${i} 2 pair)
    math(EXPR code "0x${pair}")
    if((code GREATER_EQUAL 48 AND code LESS_EQUAL 57) OR (code GREATER_EQUAL 65 AND code LESS_EQUAL 90) OR
       (code GREATER_EQUAL 97 AND code LESS_EQUAL 122) OR code EQUAL 32)
      string(ASCII ${code} c)
      string(APPEND nameLiteral "${c}")
    else()
      string(APPEND nameLiteral "\\x${pair}\" \"")
    endif()
    math(EXPR i "${i} + 2")
  endwhile()
  string(APPEND nameLiteral "\"")

  # The package, and its header against MANIFEST: magic "BSPK", sound_hash at byte 32,
  # control_hash at 64, package_hash at 96 (dsp/include/brainscape/Preset.h).
  set(bsp "${FACTORY}/${stem}.bsp")
  if(NOT EXISTS "${bsp}")
    message(FATAL_ERROR "EmbedFactory.cmake: ${bsp}, which MANIFEST lists, is missing")
  endif()
  file(SIZE "${bsp}" size)
  file(READ "${bsp}" hex HEX)
  if(size LESS 128)
    message(FATAL_ERROR "EmbedFactory.cmake: ${bsp} is shorter than a package header")
  endif()
  string(SUBSTRING "${hex}" 0 8 magic)
  string(SUBSTRING "${hex}" 64 64 headerSound)
  string(SUBSTRING "${hex}" 128 64 headerControl)
  string(SUBSTRING "${hex}" 192 64 headerPackage)
  if(NOT magic STREQUAL "4253504b")
    message(FATAL_ERROR "EmbedFactory.cmake: ${bsp} is not a package (magic ${magic})")
  endif()
  if(NOT headerPackage STREQUAL packageHash OR NOT headerSound STREQUAL soundHash OR
     NOT headerControl STREQUAL controlHash)
    message(FATAL_ERROR "EmbedFactory.cmake: ${bsp} differs from MANIFEST: its header carries "
                        "package ${headerPackage}, sound ${headerSound}, control ${headerControl}; "
                        "MANIFEST lists ${packageHash} ${soundHash} ${controlHash}")
  endif()

  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
  string(REGEX REPLACE "(${byte16})" "    \\1\n" hex "${hex}")  # 16 bytes a line
  string(APPEND arrays "// ${stem}.bsp, ${size} bytes\nalignas(8) const uint8_t kPackage${index}[] = {\n${hex}\n};\n")
  string(APPEND table "    {\"${stem}.json\", \"${id}\", ${nameLiteral}, PresetFamily::${familyEnum}, ${reserve},\n"
                      "     kPackage${index}, ${size}u,\n"
                      "     \"${packageHash}\",\n     \"${soundHash}\",\n     \"${controlHash}\"},\n")
  math(EXPR index "${index} + 1")
endforeach()
if(index EQUAL 0)
  message(FATAL_ERROR "EmbedFactory.cmake: ${FACTORY}/MANIFEST lists no package")
endif()

set(text "// Generated by plugin/EmbedFactory.cmake from firmware/factory/MANIFEST, the documents and
// the .bsp files beside them. Do not edit.
#include <cstddef>
#include <cstdint>

#include \"FactoryModes.h\"

namespace brainscape::plugin {

namespace {

${arrays}
const FactoryPackage kPackages[] = {
${table}};

}  // namespace

const FactoryPackage* FactoryPackages(size_t* count) noexcept {
  *count = sizeof kPackages / sizeof kPackages[0];
  return kPackages;
}

}  // namespace brainscape::plugin
")
if(EXISTS "${OUT}")
  file(READ "${OUT}" old)
  if(old STREQUAL text)
    return()
  endif()
endif()
file(WRITE "${OUT}" "${text}")
