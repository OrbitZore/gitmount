// gitmount — read-only git-to-FUSE filesystem (RFC 0000).
// SPDX-FileCopyrightText: 2026 The gitmount authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// GitRepo: libgit2 RAII wrapper (RFC 0000 §3.4/§5). Every libgit2 call in
// the program lives here. Callers serialize access with the single mutex
// described in §3.4 (the wrapper itself adds no locking); the three
// lock-release exceptions (revwalk chunking, oversized-blob decompression,
// LRU segmented load) are driven through callbacks/return values so the
// mutex stays under the FUSE layer's control.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <git2.h>

namespace gitmount {

// Parse a .gitmodules blob (INI-style) into path -> url. Missing url for a
// path yields the empty string; malformed lines are skipped. Pure function,
// unit tested.
std::vector<std::pair<std::string, std::string>> parse_gitmodules(const std::string& content);

// Hex string of an oid (lowercase; sha1=40 / sha256=64 chars).
std::string oid_to_hex(const git_oid& oid);

struct RefInfo {
  git_oid target{};  // direct target (symbolic refs resolved through)
  bool symbolic = false;
};

struct HeadInfo {
  enum class State { Unborn, Symbolic, Detached };
  State state = State::Unborn;
  std::string symbolic_target;  // e.g. refs/heads/main
  git_oid oid{};                // Detached: the commit oid
};

class GitRepo {
 public:
  // Opens a bare or non-bare repository. On failure returns nullptr and
  // writes a human-readable message into *err.
  static std::unique_ptr<GitRepo> open(const std::string& path, std::string* err);

  // ---- refdb -------------------------------------------------------------
  // All refs under "refs/" (full names, symbolic included, unspecified order).
  std::vector<std::string> all_refs();
  // Names in all_refs() that start with prefix + "/" (prefix itself excluded).
  std::vector<std::string> refs_under(const std::string& prefix);
  // nullopt when the ref does not exist (dangling symbolic targets resolve
  // to nullopt too).
  std::optional<RefInfo> lookup_ref(const std::string& full_name);

  // Raw symbolic-target name of a ref ("" when not symbolic).
  std::string symbolic_target(const std::string& full_name);

  HeadInfo head();

  // ---- objects -----------------------------------------------------------

  // Generic raw object read (decompresses on every call; caching is
  // gitmount's job): 0 on success with the object type and bytes, else
  // errno via git_to_errno.
  int read_object(const git_oid& oid, git_object_t* type_out, std::string* out);

  // The repository's hash type (sha1 / sha256) for raw object parsing.
  git_oid_t oid_type() const { return oid_type_; }

  // Whether the ODB holds an object with exactly type COMMIT at this oid.
  // Returns false for missing objects and non-commit types alike (§3.1:
  // /commit/<oid> does no peeling).
  bool is_commit_object(const git_oid& oid);

  // Header-only size lookup for blobs (no full decompression, RFC 0000
  // §3.2/§3.5). 0 on success; ENOENT when the object is missing.
  int blob_size(const git_oid& oid, std::uint64_t* size_out);

  // Full blob read (decompresses on every call; caching is gitmount's job).
  // 0 on success, else errno.
  int read_blob(const git_oid& oid, std::string* out);

  // ---- revwalk -----------------------------------------------------------
  // Append every reachable commit oid (hex + '\n') to *out in pinned order:
  // GIT_SORT_TOPOLOGICAL with pushes applied in the given (already sorted)
  // order, HEAD last (RFC 0000 §3.5). Every 4096 yields the lock via
  // unlock/lock callbacks (chunked revwalk, RFC 0000 §3.4). Returns 0 or
  // errno (object vanished mid-walk -> EIO semantics handled by caller).
  int list_commits(const std::vector<git_oid>& sorted_push_oids,
                   const std::function<void()>& unlock, const std::function<void()>& lock,
                   std::string* out);

  // ---- statfs ------------------------------------------------------------
  // Local ODB usage in bytes: all *.pack files plus loose object files.
  // Alternates are NOT counted (RFC 0000 §3.3); alternates_active() lets the
  // caller emit the verbose notice once.
  std::uint64_t odb_disk_bytes();
  bool alternates_active();

  const std::string& gitdir() const { return gitdir_; }
  git_repository* raw() { return repo_.get(); }

 private:
  struct FreeRepository {
    void operator()(git_repository* p) const { git_repository_free(p); }
  };
  std::unique_ptr<git_repository, FreeRepository> repo_;
  std::string gitdir_;
  git_oid_t oid_type_ = GIT_OID_SHA1;
};

// Process-wide libgit2 setup/teardown (main.cpp owns the lifetime; repo
// open/close must stay inside).
void libgit2_global_init();
void libgit2_global_shutdown();

// Apply RFC 0000 §3.5 cache tuning (as amended for the Tier-2 metadata
// caches): libgit2's parsed-object cache is fully disabled (blob, tree and
// commit per-type limits all pinned to 0) — gitmount caches raw tree bytes
// and compact commit facts itself, byte-accounted. libgit2 remains the ODB
// decompression engine, refdb and revwalk provider.
void libgit2_configure_cache();

}  // namespace gitmount
