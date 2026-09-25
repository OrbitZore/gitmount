// gitmount — read-only git-to-FUSE filesystem (RFC 0000).
// SPDX-FileCopyrightText: 2026 The gitmount authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Gitmount: fuse_operations implementation (RFC 0000 §3.1–§3.6).
//
// Concurrency model (RFC 0000 §3.4): fuse3 multithreaded loop; a single
// mutex (mu_) serializes every libgit2 call, the blob LRU, the st_ino
// registry and the /commits state. Three lock-release exceptions are
// implemented as documented:
//   * chunked revwalk (list_commits with unlock/lock callbacks)
//   * oversized-blob open-pin decompression outside the lock
//   * segmented LRU load (lookup in lock, decompress outside, re-check in)
#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>

#include "gitrepo.hpp"
#include "ino_registry.hpp"
#include "object_cache.hpp"
#include "path_map.hpp"

namespace gitmount {

class Gitmount {
 public:
  Gitmount(std::unique_ptr<GitRepo> repo, std::string repo_abs_path, std::uint64_t blob_cache_bytes,
           std::uint64_t tree_cache_bytes);

  static const fuse_operations* fuse_ops();

 private:
  // ---------------------------------------------------------------------
  // Resolution (RFC 0000 §3.1)
  // ---------------------------------------------------------------------
  struct Node {
    enum class Type {
      RootDir,          // "/" — fixed listing
      EntryDir,         // /branch /tag /commit /remote (fixed listing)
      GroupDir,         // synthesized grouping directory
      TreeDir,          // a git tree rendered as a directory
      SubmoduleDir,     // gitlink rendered as an empty directory (§3.2)
      BlobFile,         // 100644 / 100755 entry
      Symlink,          // 120000 entry
      SubmoduleMarker,  // "<name>.gitmount-submodule" synthetic file (§3.2)
      CommitsFile,
      JsonFile,
    };
    Type type = Type::RootDir;
    pathmap::Ns ns = pathmap::Ns::Branch;  // EntryDir
    git_oid tree_oid{};                    // TreeDir
    git_oid blob_oid{};                    // BlobFile / Symlink
    git_oid gitlink_oid{};                 // SubmoduleMarker: submodule commit
    git_oid commit_root_tree{};            // root tree of the owning commit (markers)
    std::string marker_path;               // SubmoduleMarker: full tree path of the submodule
    bool executable = false;
    std::int64_t commit_time = 0;  // committer time of the owning commit
    // Non-empty when this TreeDir is a complete-ref node (merged nodes
    // included): readdir unions tree entries with sub-ref first components.
    std::string ref_prefix;
    // Non-empty for GroupDir: the refs/<ns>/... prefix to enumerate below.
    std::string group_prefix;
  };

  // Resolves path against the current refdb/ODB. Returns 0 or -errno.
  // mu_ must be held on entry and is still held on return (it may be
  // dropped and re-acquired inside for segmented blob loads).
  int resolve_locked(const std::string& path, Node* out);

  int resolve_ns_locked(const std::string& ref_ns_prefix, const std::vector<std::string>& comps,
                        Node* out);
  int resolve_remote_locked(const std::vector<std::string>& comps, Node* out);
  int resolve_head_locked(const std::vector<std::string>& tree_comps, Node* out);
  int walk_tree_locked(const git_oid& root_tree, const std::vector<std::string>& comps,
                       std::int64_t commit_time, Node* out);

  // ---------------------------------------------------------------------
  // readdir enumeration
  // ---------------------------------------------------------------------
  struct DirEntry {
    std::string name;
    mode_t mode = S_IFDIR;  // type bits only
    std::uint64_t ino = 0;
  };
  // Snapshot of a directory listing, pinned to fi->fh between opendir and
  // releasedir (RFC 0000 §3.3).
  struct DirList {
    std::vector<DirEntry> entries;
  };

  // Appends the directory listing of a resolved node. Returns -errno.
  int list_dir_locked(const std::string& path, const Node& node, std::vector<DirEntry>* out);
  void append_tree_entries_locked(const git_oid& tree, const std::string& ref_prefix_for_union,
                                  std::vector<DirEntry>* out);

  // ---------------------------------------------------------------------
  // blob access (segmented locking per RFC 0000 §3.4/§3.5)
  // ---------------------------------------------------------------------
  struct BlobView {
    const std::string* cached = nullptr;  // pointer into the LRU
    std::string owned;                    // bypass copy (oversized)
    const char* data() const { return cached ? cached->data() : owned.data(); }
    std::size_t size() const { return cached ? cached->size() : owned.size(); }
  };
  // LRU path: lock, lookup, miss -> unlock/decompress/re-lock/re-check,
  // insert-or-bypass (RFC 0000 §3.4 exception 3). Each full decompression
  // logs one verbose line (decompression counter, §3.5).
  int load_blob_locked(const git_oid& oid, BlobView* view);
  // Tier-2 metadata cache accessors (RFC 0000 §3.5 as amended). Both may
  // drop and re-acquire mu_ for the segmented object read; on return mu_ is
  // held again. tree_locked returns nullptr on failure (malformed=true for
  // corrupt trees); commit_facts_locked returns 0 or -errno (-EINVAL marks
  // the non-commit-target case of §3.1).
  std::shared_ptr<const rawobj::TreeData> tree_locked(const git_oid& tree, bool* malformed);
  int commit_facts_locked(const git_oid& target, git_oid* commit_out,
                          rawobj::CommitFacts* facts_out);
  int peel_ref_to_root_locked(const git_oid& target, const std::string& refname_for_log,
                              git_oid* root_out, std::int64_t* time_out);
  // Oversized path (§3.5): decompress fully OUTSIDE the lock, caller pins
  // the result on the open handle. Returns 0 or errno.
  int decompress_blob_unlocked(const git_oid& oid, std::string* out);

  // ---------------------------------------------------------------------
  // /commits (RFC 0000 §3.5)
  // ---------------------------------------------------------------------
  std::string compute_fingerprint_locked();
  // Single-flight generation; pins a buffer snapshot into *out.
  // Returns 0 or -errno (EIO when generation fails, e.g. gc race).
  int open_commits(std::shared_ptr<const std::string>* out);

  // ---------------------------------------------------------------------
  // submodule markers (RFC 0000 §3.2)
  // ---------------------------------------------------------------------
  // Content of "<name>.gitmount-submodule": two key=value lines. Reads the
  // commit's root .gitmodules through the ordinary LRU blob path (§3.2).
  std::string submodule_marker_content_locked(const git_oid& commit_tree, const std::string& name,
                                              const git_oid& gitlink_oid);

  // ---------------------------------------------------------------------
  // metadata helpers (RFC 0000 §3.2)
  // ---------------------------------------------------------------------
  void fill_dir_stat(struct stat* st, std::int64_t mtime);
  void fill_file_stat(struct stat* st, std::int64_t mtime, std::uint64_t size, mode_t perm);

  // ---------------------------------------------------------------------
  // FUSE callbacks (static dispatch through fuse_get_context)
  // ---------------------------------------------------------------------
  static int cb_getattr(const char* path, struct stat* st, struct fuse_file_info* fi);
  static int cb_readlink(const char* path, char* buf, size_t size);
  static int cb_opendir(const char* path, struct fuse_file_info* fi);
  static int cb_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset,
                        struct fuse_file_info* fi, enum fuse_readdir_flags flags);
  static int cb_releasedir(const char* path, struct fuse_file_info* fi);
  static int cb_open(const char* path, struct fuse_file_info* fi);
  static int cb_read(const char* path, char* buf, size_t size, off_t offset,
                     struct fuse_file_info* fi);
  static int cb_release(const char* path, struct fuse_file_info* fi);
  static int cb_statfs(const char* path, struct statvfs* st);
  static int cb_erofs();

  // ---------------------------------------------------------------------
  // state
  // ---------------------------------------------------------------------
  struct Handle {
    enum class Kind { Plain, PinnedBlob, PinnedCommits, Synthetic, Dir };
    Kind kind = Kind::Plain;
    std::string data;                           // PinnedBlob
    std::shared_ptr<const std::string> shared;  // PinnedCommits / Synthetic
  };

  std::unique_ptr<GitRepo> repo_;
  std::string repo_abs_path_;    // realpath'd, for .gitmount.json
  std::string json_content_;     // .gitmount.json body (§3.6, immutable)
  std::int64_t mount_time_ = 0;  // seconds since epoch
  std::uint64_t blob_cache_bytes_;
  std::uint64_t tree_cache_bytes_;
  uid_t uid_ = 0;
  gid_t gid_ = 0;

  std::mutex mu_;  // the single mutex (RFC 0000 §3.4)
  BlobLruCache blob_cache_;
  MetaLruCache meta_cache_;  // Tier-2: raw trees + commit facts (§3.5)
  InoRegistry inos_;

  // /commits single-flight state (RFC 0000 §3.4/§3.5).
  std::mutex commits_gen_mu_;  // outer lock; never held with mu_ inner-reversed
  std::condition_variable commits_cv_;
  bool commits_generating_ = false;  // guarded by commits_gen_mu_
  std::uint64_t commits_epoch_ = 0;  // guarded by commits_gen_mu_
  // guarded by mu_:
  std::shared_ptr<const std::string> commits_buffer_;
  std::string commits_fingerprint_;
  std::int64_t commits_generated_at_ = 0;
};

// Byte-dictionary order (memcmp semantics, no locale) — RFC 0000 §3.1.
bool byte_less(const std::string& a, const std::string& b);

// %XX-escape invalid-UTF-8 bytes and JSON-quote a path for .gitmount.json
// (RFC 0000 §3.6: escaping guarantees valid JSON text, not reversibility).
std::string json_quote_path(const std::string& s);

// "2026-09-24T02:29:00Z" from a unix timestamp.
std::string iso8601_utc(std::int64_t unix_time);

}  // namespace gitmount
