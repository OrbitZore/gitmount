// gitmount — read-only git-to-FUSE filesystem (RFC 0000).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// st_ino registry (RFC 0000 §3.2).
//
// st_ino = 64-bit stable hash of the full VFS path bytes. Deliberately NOT
// oid-derived (equal inodes would make tar -c / rsync -H mistake the same
// blob under two ref paths for hard links), and inode equality never
// promises identical content (no hardlink semantics here).
//
// Collision disambiguation: a mount-lifetime registry maps primary hash →
// first occupying path. A later path whose primary hash is already taken
// takes the first free value among H(path || '#' || k), k = 1, 2, ...
// Different paths always get different inodes. The hash function is
// injectable so unit tests can force collisions (§3.2).
//
// Entries register lazily (only paths actually resolved by getattr/readdir)
// and are never recycled within a mount (an inode is never reused for a
// different object). The root path "/" is pinned to FUSE_ROOT_ID = 1 and
// occupies its slot like any other entry.
//
// NOT thread-safe by itself: guarded by the single mutex (RFC 0000 §3.4).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace gitmount {

class InoRegistry {
 public:
  using HashFn = std::function<std::uint64_t(const std::string&)>;

  static constexpr std::uint64_t kRootIno = 1;  // FUSE_ROOT_ID

  // Default hash: FNV-1a 64 over the path bytes — fixed, so inodes are
  // deterministic across remounts (except on collision paths, §3.2).
  static std::uint64_t fnv1a64(const std::string& s);

  explicit InoRegistry(HashFn hash = fnv1a64) : hash_(std::move(hash)) {}

  // Idempotent per path: registering the same path again returns the same
  // inode. Returns FUSE_ROOT_ID for "/".
  std::uint64_t register_path(const std::string& path);

  std::size_t size() const { return by_path_.size(); }

 private:
  HashFn hash_;
  std::unordered_map<std::string, std::uint64_t> by_path_;
  std::unordered_map<std::uint64_t, std::string> by_ino_;
};

}  // namespace gitmount
