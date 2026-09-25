// gitmount — read-only git-to-FUSE filesystem (RFC 0000).
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

struct TreeEntry {
  std::string name;
  git_filemode_t mode = GIT_FILEMODE_UNREADABLE;
  git_oid oid{};
};

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
  // 0 on success, else an errno (via git_to_errno). peel failure (tag chain
  // ending in tree/blob) yields EINVAL — callers treat it as the "non-commit
  // ref target" case of RFC 0000 §3.1.
  int peel_to_commit(const git_oid& target, git_oid* commit_out);
  int commit_committer_time(const git_oid& commit, std::int64_t* time_out);
  int commit_root_tree(const git_oid& commit, git_oid* tree_out);

  // Whether the ODB holds an object with exactly type COMMIT at this oid.
  // Returns false for missing objects and non-commit types alike (§3.1:
  // /commit/<oid> does no peeling).
  bool is_commit_object(const git_oid& oid);

  // Tree enumeration/lookup. tree_entries returns raw entries (including
  // pathological "." / ".." names and overlong names — filtering and warning
  // is policy in gitmount.cpp).
  std::optional<std::vector<TreeEntry>> tree_entries(const git_oid& tree);
  std::optional<TreeEntry> tree_entry(const git_oid& tree, const std::string& name);

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
};

// Process-wide libgit2 setup/teardown (main.cpp owns the lifetime; repo
// open/close must stay inside).
void libgit2_global_init();
void libgit2_global_shutdown();

// Apply RFC 0000 §3.5 cache tuning: tree/commit per-type limit raised to
// 1 MiB, blob per-type limit pinned to 0 (no double caching with gitmount's
// own LRU), total budget = tree_cache_bytes.
void libgit2_configure_cache(std::uint64_t tree_cache_bytes);

}  // namespace gitmount
