// gitmount unit tests — raw git object parsing + metadata cache (Tier 2).
// SPDX-FileCopyrightText: 2026 The gitmount authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <cstring>

#include "object_cache.hpp"
#include "rawobj.hpp"

using namespace gitmount;
using namespace gitmount::rawobj;

namespace {

git_oid oid_of(const char* hex) {
  git_oid o{};
  REQUIRE(git_oid_fromstr(&o, hex) == 0);
  return o;
}

// Serialize one entry the way git stores trees.
void append_entry(std::string* buf, const char* mode, const std::string& name, const git_oid& oid) {
  *buf += mode;
  *buf += ' ';
  *buf += name;
  *buf += '\0';
  buf->append(reinterpret_cast<const char*>(oid.id), 20);
}

TreeData make_tree(std::initializer_list<std::pair<const char*, std::string>> entries) {
  std::string raw;
  for (const auto& [mode, name] : entries)
    append_entry(&raw, mode, name, oid_of("0100000000000000000000000000000000000001"));
  TreeData td;
  td.raw = std::move(raw);
  auto idx = index_tree(td.raw.data(), td.raw.size(), GIT_OID_SHA1);
  REQUIRE(idx.has_value());
  td.offsets = std::move(*idx);
  return td;
}

const char* kOid1 = "0100000000000000000000000000000000000001";
const char* kOid2 = "0200000000000000000000000000000000000002";

}  // namespace

TEST_CASE("index_tree parses entries and offsets", "[rawobj]") {
  std::string raw;
  append_entry(&raw, "100644", "a.txt", oid_of(kOid1));
  append_entry(&raw, "040000", "sub", oid_of(kOid2));
  auto idx = index_tree(raw.data(), raw.size(), GIT_OID_SHA1);
  REQUIRE(idx.has_value());
  REQUIRE(idx->size() == 2);
  CHECK((*idx)[0] == 0);
  CHECK((*idx)[1] > 0);

  TreeData td;
  td.raw = raw;
  td.offsets = *idx;
  auto e0 = entry_at(td, (*idx)[0], GIT_OID_SHA1);
  REQUIRE(e0.has_value());
  CHECK(e0->name == "a.txt");
  CHECK(e0->mode == kModeBlob);
  const git_oid want0 = oid_of(kOid1);
  CHECK(git_oid_equal(&e0->oid, &want0));
  auto e1 = entry_at(td, (*idx)[1], GIT_OID_SHA1);
  REQUIRE(e1.has_value());
  CHECK(e1->name == "sub");
  CHECK(e1->mode == kModeTree);
  const git_oid want1 = oid_of(kOid2);
  CHECK(git_oid_equal(&e1->oid, &want1));
}

TEST_CASE("index_tree rejects malformed buffers", "[rawobj]") {
  const char* bad_modes[] = {"", "x", "100644", "999999"};
  for (const char* m : bad_modes) {
    std::string raw = m;
    raw += " name\0";
    raw.append(20, '\x01');
    CHECK_FALSE(index_tree(raw.data(), raw.size(), GIT_OID_SHA1).has_value());
  }
  // truncated oid
  std::string raw = "100644 a\0";
  raw.append(10, '\x01');
  CHECK_FALSE(index_tree(raw.data(), raw.size(), GIT_OID_SHA1).has_value());
  // no NUL in name
  std::string nonul = "100644 abcdef";
  nonul.append(20, '\x01');
  CHECK_FALSE(index_tree(nonul.data(), nonul.size(), GIT_OID_SHA1).has_value());
}

TEST_CASE("tree_find: exact match, miss, and mode dispatch", "[rawobj]") {
  auto td = make_tree({{"100644", "a"},
                       {"100755", "run"},
                       {"040000", "sub"},
                       {"120000", "lnk"},
                       {"160000", "dep"}});
  auto a = tree_find(td, "a", GIT_OID_SHA1);
  REQUIRE(a.has_value());
  CHECK(a->mode == kModeBlob);
  CHECK(tree_find(td, "run", GIT_OID_SHA1)->mode == kModeExec);
  CHECK(tree_find(td, "sub", GIT_OID_SHA1)->mode == kModeTree);
  CHECK(tree_find(td, "lnk", GIT_OID_SHA1)->mode == kModeLink);
  CHECK(tree_find(td, "dep", GIT_OID_SHA1)->mode == kModeCommit);
  CHECK_FALSE(tree_find(td, "missing", GIT_OID_SHA1).has_value());
  CHECK_FALSE(tree_find(td, "", GIT_OID_SHA1).has_value());
}

TEST_CASE("cmp_tree_name applies the '/' suffix rule", "[rawobj]") {
  // Equal bytes are a lookup match even for directory entries (libgit2
  // byname semantics; D/F uniqueness makes this unambiguous).
  CHECK(cmp_tree_name("foo", false, "foo") == 0);
  CHECK(cmp_tree_name("foo", true, "foo") == 0);
  // "foo.txt" vs dir "foo": '.' (0x2E) < '/' (0x2F) — file first.
  CHECK(cmp_tree_name("foo", true, "foo.txt") > 0);
  CHECK(cmp_tree_name("foo.txt", false, "foo") > 0);  // plain compare
  // probe is a prefix of a longer plain name.
  CHECK(cmp_tree_name("foobar", false, "foo") > 0);
  CHECK(cmp_tree_name("foo", false, "foobar") < 0);
}

TEST_CASE("tree_find binary-search path on a large tree", "[rawobj]") {
  // > kLinearScanMax entries, written in git's canonical order: plain
  // names ascending, and directory entries as if suffixed with '/'.
  std::string raw;
  std::vector<std::string> names;
  for (int i = 0; i < 200; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "f%03d", i);
    names.emplace_back(buf);
  }
  names.emplace_back("zdir");  // dir sorts after "f199" (as "zdir/")
  for (const auto& n : names)
    append_entry(&raw, n == "zdir" ? "040000" : "100644", n, oid_of(kOid1));
  TreeData td;
  td.raw = std::move(raw);
  auto idx = index_tree(td.raw.data(), td.raw.size(), GIT_OID_SHA1);
  REQUIRE(idx.has_value());
  td.offsets = std::move(*idx);
  REQUIRE(td.offsets.size() > kLinearScanMax);
  for (const auto& n : names) {
    auto e = tree_find(td, n, GIT_OID_SHA1);
    INFO(n.c_str());
    REQUIRE(e.has_value());
    CHECK(e->name == n);
  }
  CHECK(tree_find(td, "f200", GIT_OID_SHA1) == std::nullopt);
  // Under git's ordering a directory "zdir" *is* "zdir/": the suffixed
  // probe matches (unreachable from the VFS — components never contain
  // '/' — but it documents the comparator's semantics).
  REQUIRE(tree_find(td, "zdir/", GIT_OID_SHA1).has_value());
  CHECK(tree_find(td, "zdirx", GIT_OID_SHA1) == std::nullopt);
  // '.' (0x2E) sorts before "f000"?  No: '.' < 'f' — "dir0" would fit
  // before; verify a name that sorts between plain and dir forms.
  CHECK(tree_find(td, "f099x", GIT_OID_SHA1) == std::nullopt);
}

TEST_CASE("parse_commit extracts tree and committer time", "[rawobj]") {
  const char* body =
      "tree 0123456789abcdef0123456789abcdef01234567\n"
      "parent 0123456789abcdef0123456789abcdef01234567\n"
      "author A U Thor <a@example.com> 1112911993 +0200\n"
      "committer C O Mitter <c@example.com> 1136245200 -0700\n"
      "\n"
      "message body\n";
  auto f = parse_commit(body, std::strlen(body), GIT_OID_SHA1);
  REQUIRE(f.has_value());
  const git_oid want_tree = oid_of("0123456789abcdef0123456789abcdef01234567");
  CHECK(git_oid_equal(&f->root_tree, &want_tree));
  CHECK(f->committer_time == 1136245200);
}

TEST_CASE("parse_commit skips gpgsig continuation lines", "[rawobj]") {
  const char* body =
      "tree 0123456789abcdef0123456789abcdef01234567\n"
      "gpgsig -----BEGIN PGP SIGNATURE-----\n"
      " iQIzBAABCgAdFiEEgresting multiline\n"
      " =abcd\n"
      " -----END PGP SIGNATURE-----\n"
      "committer X <x@y> 1700000000 +0000\n"
      "\n"
      "msg\n";
  auto f = parse_commit(body, std::strlen(body), GIT_OID_SHA1);
  REQUIRE(f.has_value());
  CHECK(f->committer_time == 1700000000);
}

TEST_CASE("parse_commit rejects missing headers", "[rawobj]") {
  const char* no_tree = "committer X <x@y> 1700000000 +0000\n\nm\n";
  CHECK_FALSE(parse_commit(no_tree, std::strlen(no_tree), GIT_OID_SHA1).has_value());
  const char* no_committer = "tree 0123456789abcdef0123456789abcdef01234567\n\nm\n";
  CHECK_FALSE(parse_commit(no_committer, std::strlen(no_committer), GIT_OID_SHA1).has_value());
}

TEST_CASE("parse_tag extracts the target oid", "[rawobj]") {
  const char* body =
      "object 0123456789abcdef0123456789abcdef01234567\n"
      "type commit\n"
      "tag v1.0\n"
      "tagger T <t@y> 1700000000 +0000\n"
      "\n"
      "\n";
  auto o = parse_tag(body, std::strlen(body), GIT_OID_SHA1);
  REQUIRE(o.has_value());
  const git_oid want = oid_of("0123456789abcdef0123456789abcdef01234567");
  CHECK(git_oid_equal(&*o, &want));
  CHECK_FALSE(parse_tag("type commit\n", 12, GIT_OID_SHA1).has_value());
}

TEST_CASE("MetaLruCache accounts bytes exactly", "[meta_cache]") {
  MetaLruCache c(1024);
  auto mk = [](const char* payload) {
    auto mv = std::make_shared<MetaValue>();
    mv->kind = MetaValue::Kind::Tree;
    auto td = std::make_shared<TreeData>();
    td->raw = payload;
    mv->tree = td;
    return std::make_pair(mv, td->cost());
  };
  auto [v1, c1] = mk(std::string(100, 'x').c_str());
  CHECK(c.insert("t1", v1, c1));
  const std::size_t after1 = c.bytes();
  CHECK(after1 == kMetaFixedCost + c1);
  auto [v2, c2] = mk(std::string(200, 'y').c_str());
  CHECK(c.insert("t2", v2, c2));
  CHECK(c.bytes() == after1 + kMetaFixedCost + c2);
  // Hit refreshes recency and keeps accounting.
  CHECK(c.lookup("t1") != nullptr);
  CHECK(c.bytes() == after1 + kMetaFixedCost + c2);
  // Replace deducts the old cost.
  auto [v3, c3] = mk(std::string(50, 'z').c_str());
  CHECK(c.insert("t1", v3, c3));
  CHECK(c.bytes() == kMetaFixedCost + c3 + kMetaFixedCost + c2);
  // Entry at/over the whole budget is refused without evicting.
  auto [vbig, cbig] = mk(std::string(4096, '!').c_str());
  CHECK_FALSE(c.insert("big", vbig, cbig));
  CHECK(c.lookup("t1") != nullptr);
  CHECK(c.lookup("t2") != nullptr);
}

TEST_CASE("MetaLruCache evicts least-recently-used", "[meta_cache]") {
  MetaLruCache c(512);
  for (int i = 0; i < 8; ++i) {
    auto mv = std::make_shared<MetaValue>();
    auto td = std::make_shared<TreeData>();
    td->raw = std::string(40, 'a');
    mv->kind = MetaValue::Kind::Tree;
    mv->tree = td;
    std::string key = "k" + std::to_string(i);
    CHECK(c.insert(key, mv, td->cost()));
  }
  CHECK(c.entries() <= 512 / (kMetaFixedCost + 40));
  CHECK(c.bytes() <= 512);
  CHECK(c.lookup("k0") == nullptr);  // evicted long ago
  CHECK(c.lookup("k7") != nullptr);
}
