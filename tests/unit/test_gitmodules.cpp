// gitfs unit tests — .gitmodules parsing (RFC 0000 §3.2).
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "gitrepo.hpp"

using gitfs::parse_gitmodules;

TEST_CASE("parses path/url pairs", "[gitmodules]") {
  const auto m = parse_gitmodules(
      "[submodule \"deps/libfoo\"]\n"
      "\tpath = deps/libfoo\n"
      "\turl = https://example.com/libfoo.git\n"
      "[submodule \"vendor/bar\"]\n"
      "\tpath = vendor/bar\n"
      "\turl = git@example.com:bar.git\n");
  REQUIRE(m.size() == 2);
  CHECK(m[0].first == "deps/libfoo");
  CHECK(m[0].second == "https://example.com/libfoo.git");
  CHECK(m[1].first == "vendor/bar");
  CHECK(m[1].second == "git@example.com:bar.git");
}

TEST_CASE("missing url yields empty string", "[gitmodules]") {
  const auto m = parse_gitmodules(
      "[submodule \"x\"]\n"
      "	path = x\n");
  REQUIRE(m.size() == 1);
  CHECK(m[0].first == "x");
  CHECK(m[0].second.empty());
}

TEST_CASE("comments and blank lines ignored", "[gitmodules]") {
  const auto m = parse_gitmodules(
      "# top comment\n"
      "\n"
      "[submodule \"a\"] ; trailing comment\n"
      "path = a\n"
      "url = u1\n");
  REQUIRE(m.size() == 1);
  CHECK(m[0].second == "u1");
}

TEST_CASE("quoted values are unquoted", "[gitmodules]") {
  const auto m = parse_gitmodules(
      "[submodule \"a\"]\n"
      "path = \"a b\"\n"
      "url = \"https://x/with space.git\"\n");
  REQUIRE(m.size() == 1);
  CHECK(m[0].first == "a b");
  CHECK(m[0].second == "https://x/with space.git");
}

TEST_CASE("empty content yields nothing", "[gitmodules]") {
  CHECK(parse_gitmodules("").empty());
  CHECK(parse_gitmodules("garbage without sections\nkey=value\n").empty());
}

TEST_CASE("path-less sections are dropped", "[gitmodules]") {
  const auto m = parse_gitmodules("[submodule \"nopath\"]\nurl = u\n");
  CHECK(m.empty());
}
