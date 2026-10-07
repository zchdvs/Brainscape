#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Document.h"
#include "Json.h"

// Migration (docs/design/mode-compiler.md §4.4, §8.2): documents of an older schema renamed key
// by key (an empty table in schema 1), and the plugin skeleton's sessions (BSWS v1) turned into
// preset documents.
namespace bsc {

// Step 2 of §8.2: rewrites `root`, read as schema `fromVersion`, into the newest schema's keys.
// Schema 1 is the first, so there is nothing to rename yet.
void MigrateByName(json::Value* root, uint32_t fromVersion);

// A BSWS v1 session ("BSWS", u32 1, n x {u32 id, u32 bits}, m x {u32 key, u32 bits}) as a preset
// document (§4.4): IDs 1-26 by name; ID 27 at 0.5 or more lists onset among the sources; ID 28
// at 0.5 or more sets layer 0's position source to mark; macros take their defaults and their
// positions 0.5. Values are canonicalized as SetParam would. Warnings name what changes: a
// nonzero wet trim (it no longer scales the dry signal since sound revision 2), and ids this
// build does not hold as leaves. False for a malformed or newer session.
bool MigrateSession(const uint8_t* bytes, size_t length, const std::string& id,
                    const std::string& name, Document* out, std::vector<Finding>* findings);

}  // namespace bsc
