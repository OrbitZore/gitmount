// gitmount unit tests — path_map state machine (RFC 0000 §5: 重点单测).
// SPDX-FileCopyrightText: 2026 The gitmount authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "path_map.hpp"

using namespace gitmount::pathmap;

TEST_CASE("root and entry dirs parse", "[path_map]") {
  CHECK(parse("/").kind == Kind::Root);
  CHECK(parse("").kind == Kind::Invalid);  // VFS never sends this

  const Parsed b = parse("/branch");
  CHECK(b.kind == Kind::EntryDir);
  CHECK(b.ns == Ns::Branch);
  CHECK(parse("/tag").ns == Ns::Tag);
  CHECK(parse("/remote").ns == Ns::Remote);
  CHECK(parse("/commit").ns == Ns::Commit);
}

TEST_CASE("namespace paths split components", "[path_map]") {
  const Parsed p = parse("/branch/feature/x/file.txt");
  REQUIRE(p.kind == Kind::NsPath);
  CHECK(p.ns == Ns::Branch);
  REQUIRE(p.comps.size() == 3);
  CHECK(p.comps[0] == "feature");
  CHECK(p.comps[1] == "x");
  CHECK(p.comps[2] == "file.txt");
}

TEST_CASE("HEAD paths", "[path_map]") {
  CHECK(parse("/HEAD").kind == Kind::HeadRoot);
  const Parsed p = parse("/HEAD/a/b");
  REQUIRE(p.kind == Kind::HeadPath);
  REQUIRE(p.comps.size() == 2);
  CHECK(p.comps[0] == "a");
}

TEST_CASE("synthetic files", "[path_map]") {
  CHECK(parse("/commits").kind == Kind::CommitsFile);
  CHECK(parse("/.gitmount.json").kind == Kind::MetaJson);
  CHECK(parse("/commits/x").err == ENOTDIR);
  CHECK(parse("/.gitmount.json/x").err == ENOTDIR);
}

TEST_CASE("unknown top-level entries are ENOENT", "[path_map]") {
  const Parsed p = parse("/objects");
  CHECK(p.kind == Kind::Invalid);
  CHECK(p.err == ENOENT);
  CHECK(parse("/ref").err == ENOENT);
}

TEST_CASE("overlong components are ENAMETOOLONG", "[path_map]") {
  std::string name(kNameMax + 1, 'a');
  const Parsed p = parse("/branch/" + name);
  CHECK(p.kind == Kind::Invalid);
  CHECK(p.err == ENAMETOOLONG);
  // NAME_MAX itself is fine syntactically.
  CHECK(parse("/branch/" + std::string(kNameMax, 'a')).kind == Kind::NsPath);
}

TEST_CASE("empty components normalize away", "[path_map]") {
  CHECK(parse("/branch//main").kind == Kind::NsPath);
  CHECK(parse("/branch/").kind == Kind::EntryDir);
  CHECK(parse("//").kind == Kind::Root);
  CHECK(parse("/").kind == Kind::Root);
}

TEST_CASE("non-UTF-8 and unicode bytes pass through untouched", "[path_map]") {
  const std::string name = "\xff\xfe";
  const Parsed p = parse("/branch/" + name);
  REQUIRE(p.kind == Kind::NsPath);
  CHECK(p.comps[0] == name);

  const Parsed uni = parse("/branch/\xe4\xb8\xad\xe6\x96\x87");
  REQUIRE(uni.kind == Kind::NsPath);
  CHECK(uni.comps[0] == "\xe4\xb8\xad\xe6\x96\x87");
}

TEST_CASE("is_full_oid accepts only full lowercase hex", "[path_map]") {
  const std::string sha1_40 = "0123456789abcdef0123456789abcdef01234567";
  const std::string sha256_64 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  CHECK(is_full_oid(sha1_40));
  CHECK(is_full_oid(sha256_64));
  CHECK_FALSE(is_full_oid(sha1_40.substr(0, 39)));                        // too short
  CHECK_FALSE(is_full_oid(sha1_40 + "00"));                               // 42 chars
  CHECK_FALSE(is_full_oid("ABCDEF0123456789abcdef0123456789abcdef01"));   // upper
  CHECK_FALSE(is_full_oid("0123456789gabcdef0123456789abcdef01234567"));  // 'g'
  CHECK_FALSE(is_full_oid(""));
}
