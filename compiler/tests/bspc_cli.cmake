# bspc's command line (docs/design/mode-compiler.md §8.2): options a command does not take are
# refused with exit 2 and touch no file, so a misspelled gate (fmt --check, lint --factory,
# stamp --check) can neither pass nor rewrite what it checks; derive says when the stamp goes
# stale; roundtrip reports a stale stamp once and names the documents a manifest disagrees on;
# file names outside the ANSI code page open on Windows (bspc's UTF-8 manifest).
#
#   cmake -DBSPC=path/to/bspc -DDATA=compiler/tests/data -DWORK=scratch/dir -P bspc_cli.cmake
foreach(var BSPC DATA WORK)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "bspc_cli.cmake needs -D${var}=...")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(failures 0)

# run(<expected exit code> args...): sets OUT and ERR in the caller.
function(run expected)
  execute_process(COMMAND "${BSPC}" ${ARGN}
                  WORKING_DIRECTORY "${WORK}"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc STREQUAL "${expected}")
    message(SEND_ERROR "bspc ${ARGN}: exit ${rc}, expected ${expected}\nstdout: ${out}\nstderr: ${err}")
  endif()
  set(OUT "${out}" PARENT_SCOPE)
  set(ERR "${err}" PARENT_SCOPE)
endfunction()

function(expect_in haystack needle what)
  string(FIND "${haystack}" "${needle}" at)
  if(at EQUAL -1)
    message(SEND_ERROR "${what}: `${needle}` not in:\n${haystack}")
  endif()
endfunction()

function(expect_not_in haystack needle what)
  string(FIND "${haystack}" "${needle}" at)
  if(NOT at EQUAL -1)
    message(SEND_ERROR "${what}: `${needle}` unexpected in:\n${haystack}")
  endif()
endfunction()

function(expect_same file hash what)
  file(SHA256 "${WORK}/${file}" now)
  if(NOT now STREQUAL hash)
    message(SEND_ERROR "${what}: ${file} was rewritten")
  endif()
endfunction()

function(expect_absent file what)
  if(EXISTS "${WORK}/${file}")
    message(SEND_ERROR "${what}: ${file} was written")
  endif()
endfunction()

# A document that is not in canonical form, so fmt and stamp would rewrite it.
file(WRITE "${WORK}/messy.json" "{\"schema_version\": 1, \"id\": \"test.cli\", \"name\": \"Cli\"}")
file(SHA256 "${WORK}/messy.json" messy)
file(COPY_FILE "${DATA}/default.json" "${WORK}/default.json")

# ── Unknown, misplaced and malformed options: exit 2, nothing written ──────────────────────
run(2 fmt --chek messy.json)
expect_in("${ERR}" "bspc fmt: unknown option `--chek` (fmt takes --check)" "fmt --chek")
expect_same(messy.json "${messy}" "fmt --chek")
run(2 stamp --dry-run messy.json)
expect_in("${ERR}" "unknown option `--dry-run`" "stamp --dry-run")
expect_same(messy.json "${messy}" "stamp --dry-run")
run(2 fmt -o out.json messy.json)
expect_same(messy.json "${messy}" "fmt -o")
expect_absent(out.json "fmt -o")
run(2 compile --check messy.json)
expect_absent(messy.bsp "compile --check")
run(2 lint --facotry default.json)
expect_in("${ERR}" "(lint takes --factory)" "lint --facotry")
run(2 compile messy.json -o)
expect_in("${ERR}" "-o needs a value" "compile -o")
run(2 compile -o a.bsp -o b.bsp messy.json)
expect_in("${ERR}" "-o is given twice" "compile -o -o")
expect_absent(a.bsp "compile -o -o")
run(2 verify --rebuild x.bsp)
expect_in("${ERR}" "(verify takes no options)" "verify --rebuild")
run(2 frobnicate messy.json)
expect_in("${ERR}" "bspc: unknown command `frobnicate`" "frobnicate")
run(2 version extra)

# ── The options each command takes still work ─────────────────────────────────────────────
run(1 fmt --check messy.json)
expect_in("${OUT}" "messy.json" "fmt --check")
expect_same(messy.json "${messy}" "fmt --check")
run(0 lint default.json)  # warnings only
run(1 lint --factory default.json)  # L4 is an error for factory presets
run(0 compile -o messy-out.bsp messy.json)
expect_absent(messy.bsp "compile -o")
run(0 compile -- messy.json)
file(REMOVE "${WORK}/messy.bsp")  # roundtrip would hold the document to it

# ── Writing a preset: fmt, derive, stamp, lint --factory, roundtrip ───────────────────────
run(0 fmt messy.json)
run(0 derive messy.json)
expect_in("${ERR}" "messy.json: it has no stamp (run bspc stamp)" "derive")
run(1 roundtrip messy.json)
expect_in("${ERR}" "its stamp is missing or stale" "roundtrip, stale")
expect_not_in("${ERR}" "rebuilt without its JSON section" "roundtrip, stale")
run(1 stamp --check messy.json)
run(0 stamp messy.json)
run(0 stamp --check messy.json)
run(0 lint --factory messy.json)
run(0 roundtrip --write-manifest manifest.txt messy.json)
file(SHA256 "${WORK}/messy.json" derived)
run(0 derive --solve messy.json)  # a derived document's positions already give its leaves
expect_same(messy.json "${derived}" "derive --solve")
run(0 derive messy.json)
expect_not_in("${ERR}" "stamp" "derive of a derived document")

# A manifest that disagrees names the document and the hashes.
file(READ "${WORK}/messy.json" text)
string(REPLACE "\"mix\": 0.5" "\"mix\": 0.25" text "${text}")
file(WRITE "${WORK}/messy.json" "${text}")
run(0 stamp messy.json)
run(1 roundtrip --expect manifest.txt messy.json)
expect_in("${ERR}" "messy.json: package hash, sound_hash changed" "roundtrip --expect")
run(0 derive messy.json)  # the edited mixes are Space's targets: derived again, stamp stale
expect_in("${ERR}" "messy.json: its stamp is stale (run bspc stamp)" "derive, stamped")
run(0 stamp messy.json)

# ── File names outside the ANSI code page ─────────────────────────────────────────────────
foreach(name "日本" "café" "пресет")
  file(COPY_FILE "${WORK}/messy.json" "${WORK}/${name}.json")
  run(0 compile "${name}.json")
  if(NOT EXISTS "${WORK}/${name}.bsp")
    message(SEND_ERROR "compile ${name}.json wrote no ${name}.bsp")
  endif()
  run(0 roundtrip "${name}.json")
endforeach()
