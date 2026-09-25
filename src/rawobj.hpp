// gitmount — read-only git-to-FUSE filesystem (RFC 0000).
// SPDX-FileCopyrightText: 2026 The gitmount authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Raw git object parsing (Tier-2 metadata caches, RFC 0000 §3.5 as
// amended): tree entries, commit headers and tag headers are parsed
// directly from the serialized bytes gitmount caches, instead of
// holding libgit2's parsed representations (which retain the raw buffer
// *plus* per-entry structs, 3-5x the serialized size).
//
// Everything here is a pure function over byte buffers — heavily unit
// tested (RFC 0000 §5), no libgit2 calls beyond git_oid helpers.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <git2.h>

namespace gitmount::rawobj {

// git tree entry modes (RFC 0000 §3.2 table 1).
constexpr std::uint32_t kModeTree = 0040000;
constexpr std::uint32_t kModeBlob = 0100644;
constexpr std::uint32_t kModeExec = 0100755;
constexpr std::uint32_t kModeLink = 0120000;
constexpr std::uint32_t kModeCommit = 0160000;

// One tree entry; `name` points into the tree's raw buffer (zero copy).
struct TreeEnt {
  std::string_view name;
  std::uint32_t mode = 0;
  git_oid oid{};
};

// Byte-offset index over a serialized tree: 4 bytes per entry, the only
// structure kept alongside the raw bytes (keeps Tier-2 accounting at
// ~1x serialized size instead of libgit2's parsed 3-5x).
using TreeIndex = std::vector<std::uint32_t>;

struct TreeData {
  std::string raw;    // serialized tree bytes (owns all entry names)
  TreeIndex offsets;  // byte offset of each entry start
  std::size_t cost() const { return raw.size() + offsets.size() * sizeof(std::uint32_t); }
};

// Build the offset index over a serialized tree. Returns nullopt on a
// malformed buffer (bad mode, missing NUL, truncated oid) — callers map
// that to ENOENT plus a warning, matching the previous libgit2 behavior.
std::optional<TreeIndex> index_tree(const char* data, std::size_t len, git_oid_t oid_type);

// Parse the entry at `off` inside a tree buffer previously validated by
// index_tree. Returns nullopt if `off` is out of range.
std::optional<TreeEnt> entry_at(const TreeData& tree, std::uint32_t off, git_oid_t oid_type);

// Find an entry by exact name, whatever its kind (file, symlink, gitlink
// or directory). Trees with more than kLinearScanMax entries binary-search
// TWICE under git's tree ordering — with the bare-name key (a file victim
// sorts before all name-extending siblings) and with the directory-slot
// key "name/" (a directory victim sorts after siblings extending it with
// bytes below '/'); D/F uniqueness means at most one pass can hit.
// Smaller trees scan linearly, which needs no ordering assumption.
constexpr std::size_t kLinearScanMax = 64;
std::optional<TreeEnt> tree_find(const TreeData& tree, std::string_view name, git_oid_t oid_type);

// Compare a probe name against an entry name under git's tree order
// (directory names compare as if suffixed with '/'). `tree_slot` selects
// the probe's effective sort key: its directory slot "name/" (looking
// for a directory) or the bare name (file/symlink/gitlink entries, whose
// stored key has no suffix). The probe's position differs between the
// two keys, which is why tree_find runs both passes. Exposed for tests.
int cmp_tree_name(std::string_view entry_name, bool entry_is_tree, std::string_view probe,
                  bool tree_slot);

// Hot-path commit facts: root tree oid + committer time (seconds).
// Parents are deliberately not kept — the revision walk uses libgit2's
// own parsing and nothing on the mount path needs them.
struct CommitFacts {
  git_oid root_tree{};
  std::int64_t committer_time = 0;
};

// Parse a commit object header (stops at the blank line; gpgsig
// continuation lines starting with a space are skipped). Returns
// nullopt when `tree` or `committer` headers are missing/malformed.
std::optional<CommitFacts> parse_commit(const char* data, std::size_t len, git_oid_t oid_type);

// Tag object header: target oid (the "object <hex>" line).
std::optional<git_oid> parse_tag(const char* data, std::size_t len, git_oid_t oid_type);

}  // namespace gitmount::rawobj
