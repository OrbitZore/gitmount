// gitmount — read-only git-to-FUSE filesystem (RFC 0000).
// SPDX-FileCopyrightText: 2026 The gitmount authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ino_registry.hpp"

namespace gitmount {

std::uint64_t InoRegistry::fnv1a64(const std::string& s) {
  const std::uint64_t kOffsetBasis = 14695981039346656037ULL;
  const std::uint64_t kPrime = 1099511628211ULL;
  std::uint64_t h = kOffsetBasis;
  for (unsigned char c : s) {
    h ^= c;
    h *= kPrime;
  }
  return h;
}

std::uint64_t InoRegistry::register_path(const std::string& path) {
  auto known = by_path_.find(path);
  if (known != by_path_.end()) return known->second;

  if (path == "/") {
    // Pinned root ino; occupies the registry slot so a path hashing to 1
    // gets disambiguated (RFC 0000 §3.2).
    by_path_[path] = kRootIno;
    by_ino_[kRootIno] = path;
    return kRootIno;
  }

  const std::uint64_t primary = hash_(path);
  if (by_ino_.find(primary) == by_ino_.end()) {
    by_path_[path] = primary;
    by_ino_[primary] = path;
    return primary;
  }

  // Collision: first free value among H(path || '#' || k), k = 1, 2, ...
  // (RFC 0000 §3.2). Degenerate injected hashes (every value equal) fall
  // back to linear probing from the primary hash so distinct paths always
  // get distinct inodes — the registry's core invariant.
  for (std::uint64_t k = 1; k < 1024; ++k) {
    const std::uint64_t candidate = hash_(path + "#" + std::to_string(k));
    if (by_ino_.find(candidate) == by_ino_.end()) {
      by_path_[path] = candidate;
      by_ino_[candidate] = path;
      return candidate;
    }
  }
  for (std::uint64_t k = 1;; ++k) {
    const std::uint64_t candidate = primary + k;
    if (candidate != 0 && by_ino_.find(candidate) == by_ino_.end()) {
      by_path_[path] = candidate;
      by_ino_[candidate] = path;
      return candidate;
    }
  }
}

}  // namespace gitmount
