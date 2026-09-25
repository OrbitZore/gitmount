// gitmount — read-only git-to-FUSE filesystem (RFC 0000).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Blob LRU cache (RFC 0000 §3.5).
//
// Key = raw bytes of the blob's git_oid (immutable, content-addressed; no
// invalidation is ever needed within a mount). Value = the uncompressed
// blob bytes. Capacity is accounted in value bytes.
//
// Pinned semantics:
//   * lookup() moves the entry to most-recently-used and returns a pointer
//     into the cache; the pointer stays valid until the next cache mutation
//     (eviction/insert). Callers copy out under the filesystem-wide mutex.
//   * insert() refuses a value whose size is >= the capacity (§3.5: the
//     boundary is deliberately >= — a blob exactly at the limit would evict
//     the whole pool to almost no benefit; oversized blobs never enter the
//     pool and never trigger eviction of existing entries).
//   * insert() of an existing key replaces the value (same-refill path).
//
// NOT thread-safe by itself: all access happens under the single mutex
// described in RFC 0000 §3.4. The segmented load (lock / decompress outside
// the lock / re-lock and re-check) lives in gitmount.cpp.
#pragma once

#include <cstddef>
#include <list>
#include <string>
#include <unordered_map>

namespace gitmount {

class BlobLruCache {
 public:
  explicit BlobLruCache(std::size_t capacity_bytes) : capacity_(capacity_bytes) {}

  std::size_t capacity() const { return capacity_; }
  std::size_t bytes() const { return bytes_; }
  std::size_t entries() const { return index_.size(); }

  // Returns nullptr on miss. A hit refreshes recency.
  const std::string* lookup(const std::string& key) {
    auto it = index_.find(key);
    if (it == index_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second);
    return &lru_.front().value;
  }

  // Returns false (and leaves the cache untouched) when value.size() >=
  // capacity, or when the value would require evicting entries and still not
  // fit — the latter is impossible for values < capacity, so eviction only
  // happens for insertable sizes.
  bool insert(const std::string& key, std::string value) {
    if (value.size() >= capacity_) return false;  // never evict for oversize
    auto it = index_.find(key);
    if (it != index_.end()) {
      bytes_ -= it->second->value.size();
      lru_.erase(it->second);
      index_.erase(it);
    }
    while (bytes_ + value.size() > capacity_ && !lru_.empty()) {
      bytes_ -= lru_.back().value.size();
      index_.erase(lru_.back().key);
      lru_.pop_back();
    }
    lru_.push_front(Entry{key, std::move(value)});
    bytes_ += lru_.front().value.size();
    index_[key] = lru_.begin();
    return true;
  }

  void clear() {
    lru_.clear();
    index_.clear();
    bytes_ = 0;
  }

 private:
  struct Entry {
    std::string key;
    std::string value;
  };

  std::size_t capacity_;
  std::size_t bytes_ = 0;
  std::list<Entry> lru_;  // front = most recently used
  std::unordered_map<std::string, std::list<Entry>::iterator> index_;
};

}  // namespace gitmount
