// gitfs — read-only git-to-FUSE filesystem (RFC 0000).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Path → parse state machine (RFC 0000 §3.1). Pure syntax-level mapping:
// everything that needs the refdb/ODB is resolved later in gitfs.cpp. This
// split keeps the parser a pure function (RFC 0000 §5: heavily unit tested).
//
// Grammar (component = non-empty byte string without '/'):
//
//   "/"                          → Root
//   "/branch" "/tag"             → EntryDir(ns)          (readdir: refs)
//   "/commit" "/remote"          → EntryDir(ns)          (/commit: empty)
//   "/branch/<c>/<c>/..."        → NsPath(ns, comps)
//   "/HEAD"                      → HeadRoot
//   "/HEAD/<c>/..."              → HeadPath(comps)
//   "/commits"                   → CommitsFile
//   "/.gitfs.json"               → MetaJson
//
// Rules pinned by RFC 0000 §3.1/§3.3:
//   * a component longer than NAME_MAX (255) → err = ENAMETOOLONG
//   * empty components (double slashes, trailing slash beyond root) are
//     normalized away — the kernel never sends them, direct callers may;
//     a path of only slashes is Root
//   * any unknown first component → err = ENOENT
//   * /commit/<oid> oid validation (full lowercase hex, 40 or 64 chars)
//     is exposed as is_full_oid() for the resolver; uppercase/invalid oids
//     normalize to ENOENT there, matching the commits list (lowercase)
#pragma once

#include <string>
#include <vector>

namespace gitfs::pathmap {

enum class Kind {
  Invalid,   // err carries the errno
  Root,      // "/"
  EntryDir,  // /branch /tag /commit /remote
  NsPath,    // components below an entry dir (ref name + tree path)
  HeadRoot,  // "/HEAD"
  HeadPath,  // components below /HEAD (tree path)
  CommitsFile,
  MetaJson,
};

enum class Ns { Branch, Tag, Remote, Commit };

struct Parsed {
  Kind kind = Kind::Invalid;
  int err = 0;                     // set only when kind == Invalid
  Ns ns = Ns::Branch;              // EntryDir / NsPath
  std::vector<std::string> comps;  // NsPath: ref components + tree path;
                                   // HeadPath: tree path
};

// Parse a VFS path (bytes, not necessarily UTF-8). Never throws.
Parsed parse(const std::string& path);

// True iff s is a full lowercase hexadecimal git oid: exactly 40 (sha1) or
// 64 (sha256) chars from [0-9a-f] (RFC 0000 §3.1).
bool is_full_oid(const std::string& s);

constexpr std::size_t kNameMax = 255;

const char* ns_dir_name(Ns ns);

}  // namespace gitfs::pathmap
