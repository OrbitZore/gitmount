// gitmount unit tests — st_ino registry (RFC 0000 §3.2).
// SPDX-FileCopyrightText: 2026 The gitmount authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <set>

#include "ino_registry.hpp"

using gitmount::InoRegistry;

TEST_CASE("root is pinned to FUSE_ROOT_ID", "[ino]") {
  InoRegistry r;
  CHECK(r.register_path("/") == InoRegistry::kRootIno);
  CHECK(r.register_path("/") == InoRegistry::kRootIno);  // idempotent
}

TEST_CASE("default hash is deterministic across instances", "[ino]") {
  InoRegistry a, b;
  for (const char* p : {"/branch/main", "/tag/v1.0", "/HEAD/a/b.txt", "/commit/deadbeef",
                        "/commits", "/.gitmount.json"}) {
    CHECK(a.register_path(p) == b.register_path(p));
  }
}

TEST_CASE("same path returns the same ino; different paths differ", "[ino]") {
  InoRegistry r;
  std::set<std::uint64_t> seen;
  const char* paths[] = {"/branch/main", "/tag/v1", "/tag/v2", "/branch/feature/x", "/HEAD"};
  for (const char* p : paths) {
    const auto ino = r.register_path(p);
    CHECK(ino != InoRegistry::kRootIno);  // only "/" is 1
    CHECK(seen.insert(ino).second);
    CHECK(r.register_path(p) == ino);
  }
}

TEST_CASE("collisions disambiguate via H(path||'#'||k)", "[ino]") {
  // Injectable hash forcing "/a" and "/b" to share a primary hash (RFC
  // 0000 §3.2: unit tests may force collisions).
  const auto collide = [](const std::string& s) -> std::uint64_t {
    if (s == "/a" || s == "/b") return 42;
    return InoRegistry::fnv1a64(s);
  };
  InoRegistry r(collide);
  const auto a = r.register_path("/a");
  const auto b = r.register_path("/b");
  CHECK(a == 42);
  CHECK(b != 42);
  CHECK(b != InoRegistry::kRootIno);
  // Idempotent after disambiguation.
  CHECK(r.register_path("/b") == b);
  // A third colliding path takes the next free value.
  const auto collide3 = [](const std::string& s) -> std::uint64_t {
    if (s == "/a" || s == "/b" || s == "/c") return 42;
    return InoRegistry::fnv1a64(s);
  };
  InoRegistry r3(collide3);
  r3.register_path("/a");
  const auto c = r3.register_path("/c");
  CHECK(c != 42);
}

TEST_CASE("a path hashing to FUSE_ROOT_ID is disambiguated away from 1", "[ino]") {
  const auto hash_to_one = [](const std::string&) -> std::uint64_t {
    return 1;  // every path "collides" with the root slot
  };
  InoRegistry r(hash_to_one);
  CHECK(r.register_path("/") == 1);
  const auto x = r.register_path("/branch/main");
  CHECK(x != 1);
  CHECK(r.register_path("/branch/main") == x);
}

TEST_CASE("registry never recycles: inos stay distinct as paths accumulate", "[ino]") {
  InoRegistry r;
  std::set<std::uint64_t> inos;
  for (int i = 0; i < 1000; ++i) {
    const auto p = "/branch/b" + std::to_string(i);
    CHECK(inos.insert(r.register_path(p)).second);
  }
}
