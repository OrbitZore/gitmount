// gitfs unit tests — blob LRU (RFC 0000 §3.5).
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "object_cache.hpp"

using gitfs::BlobLruCache;

TEST_CASE("miss then hit", "[lru]") {
  BlobLruCache c(1024);
  CHECK(c.lookup("a") == nullptr);
  CHECK(c.insert("a", std::string(10, 'x')));
  const std::string* v = c.lookup("a");
  REQUIRE(v != nullptr);
  CHECK(v->size() == 10);
  CHECK(c.bytes() == 10);
  CHECK(c.entries() == 1);
}

TEST_CASE("eviction is least-recently-used", "[lru]") {
  BlobLruCache c(30);
  c.insert("a", std::string(10, 'a'));
  c.insert("b", std::string(10, 'b'));
  c.insert("c", std::string(10, 'c'));
  CHECK(c.lookup("a") != nullptr);      // refresh a: order now b,c,a (lru first)
  c.insert("d", std::string(10, 'd'));  // evicts b
  CHECK(c.lookup("b") == nullptr);
  CHECK(c.lookup("a") != nullptr);
  CHECK(c.lookup("c") != nullptr);
  CHECK(c.lookup("d") != nullptr);
  CHECK(c.entries() == 3);
}

TEST_CASE("oversized values never enter the pool", "[lru]") {
  BlobLruCache c(100);
  // Boundary is deliberately >= (RFC 0000 §3.5): a value exactly at the
  // capacity would evict the whole pool for no benefit.
  CHECK_FALSE(c.insert("big", std::string(100, 'x')));
  CHECK_FALSE(c.insert("bigger", std::string(101, 'x')));
  CHECK(c.entries() == 0);
  CHECK(c.bytes() == 0);

  c.insert("small", std::string(10, 's'));
  // An oversized insert must not evict existing entries either.
  CHECK_FALSE(c.insert("big", std::string(150, 'x')));
  CHECK(c.lookup("small") != nullptr);
}

TEST_CASE("insert replaces existing keys", "[lru]") {
  BlobLruCache c(1024);
  c.insert("a", std::string(10, 'x'));
  c.insert("a", std::string(20, 'y'));
  const std::string* v = c.lookup("a");
  REQUIRE(v != nullptr);
  CHECK(v->size() == 20);
  CHECK(c.bytes() == 20);
  CHECK(c.entries() == 1);
}

TEST_CASE("clear resets accounting", "[lru]") {
  BlobLruCache c(1024);
  c.insert("a", std::string(10, 'x'));
  c.clear();
  CHECK(c.lookup("a") == nullptr);
  CHECK(c.bytes() == 0);
  CHECK(c.entries() == 0);
}

TEST_CASE("eviction accounting stays exact under mixed sizes", "[lru]") {
  BlobLruCache c(25);
  c.insert("a", std::string(10, 'a'));
  c.insert("b", std::string(10, 'b'));
  c.insert("c", std::string(10, 'c'));  // evicts a (10+10+10 > 25)
  CHECK(c.lookup("a") == nullptr);
  CHECK(c.bytes() == 20);
  // A value exactly at the capacity is refused by the >= boundary (§3.5).
  CHECK_FALSE(c.insert("exact", std::string(25, 'e')));
  CHECK(c.lookup("b") != nullptr);
  c.insert("d", std::string(24, 'd'));  // 20+24 > 25: evicts b and c
  CHECK(c.lookup("b") == nullptr);
  CHECK(c.lookup("c") == nullptr);
  CHECK(c.lookup("d") != nullptr);
  CHECK(c.bytes() == 24);
}
