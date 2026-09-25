// gitfs unit tests — CLI / mount(8) helper option parsing (RFC 0000 §3.7).
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "options.hpp"

using namespace gitfs::cli;

namespace {

ParseResult run(std::vector<std::string> args) {
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>("mount.gitfs"));
  for (auto& a : args) argv.push_back(const_cast<char*>(a.data()));
  return parse(static_cast<int>(argv.size()), argv.data());
}

}  // namespace

TEST_CASE("plain invocation parses two positionals", "[options]") {
  auto r = run({"/repo", "/mnt"});
  REQUIRE(r.action == Action::Run);
  CHECK(r.options.repository == "/repo");
  CHECK(r.options.mountpoint == "/mnt");
  CHECK(r.options.blob_cache_bytes == 64ULL << 20);
  CHECK(r.options.tree_cache_bytes == 256ULL << 20);
  CHECK(!r.options.fake);
  CHECK(!r.options.foreground);
}

TEST_CASE("options after positional arguments (mount(8) exec order)", "[options]") {
  // Measured helper argv: "src dir -f -o rw" (RFC 0000 §3.7).
  auto r = run({"/repo", "/mnt", "-f", "-o", "rw"});
  REQUIRE(r.action == Action::Run);
  CHECK(r.options.fake);
  CHECK(r.options.repository == "/repo");
}

TEST_CASE("missing or extra positionals fail with exit 1", "[options]") {
  CHECK(run({"/repo"}).exit_code == 1);
  CHECK(run({}).exit_code == 1);
  CHECK(run({"/repo", "/mnt", "extra"}).exit_code == 1);
}

TEST_CASE("rw is accepted as a no-op with a warning", "[options]") {
  auto r = run({"/repo", "/mnt", "-o", "rw"});
  REQUIRE(r.action == Action::Run);
  REQUIRE(r.options.warnings.size() == 1);
  CHECK(r.options.warnings[0].find("rw") != std::string::npos);
}

TEST_CASE("baseline synonyms are accepted", "[options]") {
  for (const char* key : {"ro", "nosuid", "nodev", "default_permissions", "use_ino"}) {
    auto r = run({"/repo", "/mnt", "-o", key});
    INFO(key);
    REQUIRE(r.action == Action::Run);
  }
  // Valued forms via key cut (ro=vfs, rw=fs) hit the same paths.
  CHECK(run({"/repo", "/mnt", "-o", "ro=vfs"}).action == Action::Run);
  CHECK(run({"/repo", "/mnt", "-o", "rw=fs"}).action == Action::Run);
}

TEST_CASE("enumerate-track keys are accepted and ignored", "[options]") {
  for (const char* key :
       {"atime", "noatime",  "relatime", "strictatime", "lazytime",    "diratime", "nodiratime",
        "sync",  "async",    "dirsync",  "exec",        "noexec",      "user",     "users",
        "owner", "group",    "nouser",   "symfollow",   "nosymfollow", "iversion", "silent",
        "loud",  "mand",     "nomand",   "nofs",        "_netdev",     "nofail",   "acl",
        "quiet", "showexec", "bsdgroups"}) {
    auto r = run({"/repo", "/mnt", "-o", key});
    INFO(key);
    REQUIRE(r.action == Action::Run);
    CHECK(r.options.fuse_passthrough.empty());
  }
  // Table-track no-ops (negated forms of table keys).
  for (const char* key : {"noiversion", "norelatime", "nostrictatime", "nolazytime"}) {
    auto r = run({"/repo", "/mnt", "-o", key});
    INFO(key);
    REQUIRE(r.action == Action::Run);
  }
  // Valued forms: the key name decides (rule 1, RFC 0000 §3.7).
  CHECK(run({"/repo", "/mnt", "-o", "user=alice"}).action == Action::Run);
  CHECK(run({"/repo", "/mnt", "-o", "nofail=1"}).action == Action::Run);
}

TEST_CASE("reject-track keys fail with dedicated errors and exit 1", "[options]") {
  for (const char* key : {"suid", "dev", "remount", "uid=1000", "gid=1000", "umask=022",
                          "context=system_u:object_r:user_home_t:s0", "fscontext=x", "defcontext=x",
                          "rootcontext=x"}) {
    auto r = run({"/repo", "/mnt", "-o", key});
    INFO(key);
    REQUIRE(r.action == Action::Fail);
    CHECK(r.exit_code == 1);
    CHECK(!r.message.empty());
  }
}

TEST_CASE("own keys parse with hyphen/underscore equivalence and last-wins", "[options]") {
  auto r = run({"/repo", "/mnt", "-o", "blob-cache-size=128"});
  REQUIRE(r.action == Action::Run);
  CHECK(r.options.blob_cache_bytes == 128ULL << 20);

  r = run({"/repo", "/mnt", "-o", "blob_cache_size=32"});
  CHECK(r.options.blob_cache_bytes == 32ULL << 20);

  // Repeated key: the later occurrence wins (RFC 0000 §3.7).
  r = run({"/repo", "/mnt", "-o", "blob-cache-size=128,blob-cache-size=256"});
  CHECK(r.options.blob_cache_bytes == 256ULL << 20);

  r = run({"/repo", "/mnt", "--blob-cache-size", "300"});
  CHECK(r.options.blob_cache_bytes == 300ULL << 20);
  r = run({"/repo", "/mnt", "--tree-cache-size", "5"});
  CHECK(r.options.tree_cache_bytes == 5ULL << 20);
}

TEST_CASE("cache sizes must be positive integers", "[options]") {
  for (const char* bad : {"0", "-1", "abc", "", "1.5", " 8", "99999999999999999999"}) {
    auto r = run({"/repo", "/mnt", "-o", std::string("blob-cache-size=") + bad});
    INFO(bad);
    REQUIRE(r.action == Action::Fail);
    CHECK(r.exit_code == 1);
  }
}

TEST_CASE("fsname overrides; subtype is protected", "[options]") {
  auto r = run({"/repo", "/mnt", "-o", "fsname=myrepo"});
  REQUIRE(r.action == Action::Run);
  CHECK(r.options.fsname == "myrepo");

  r = run({"/repo", "/mnt", "-o", "subtype=evil"});
  REQUIRE(r.action == Action::Fail);
  CHECK(r.exit_code == 1);
}

TEST_CASE("unknown keys pass through to libfuse", "[options]") {
  auto r = run({"/repo", "/mnt", "-o", "kernel_cache,allow_other"});
  REQUIRE(r.action == Action::Run);
  REQUIRE(r.options.fuse_passthrough.size() == 2);
  CHECK(r.options.fuse_passthrough[0] == "kernel_cache");
  CHECK(r.options.fuse_passthrough[1] == "allow_other");
}

TEST_CASE("comma splitting keeps colon-bearing values whole", "[options]") {
  // context= values contain colons; only commas split (rule 1 caveat).
  auto r = run({"/repo", "/mnt", "-o", "context=system_u:object_r:user_home_t:s0,noatime"});
  REQUIRE(r.action == Action::Fail);  // context= is on the reject track
  // A benign colon value passes through whole.
  r = run({"/repo", "/mnt", "-o", "x-foo=a:b"});
  REQUIRE(r.action == Action::Run);
  REQUIRE(r.options.fuse_passthrough.size() == 1);
  CHECK(r.options.fuse_passthrough[0] == "x-foo=a:b");
}

TEST_CASE("-t gitfs accepted; anything else is a fstype mismatch", "[options]") {
  CHECK(run({"/repo", "/mnt", "-t", "gitfs"}).action == Action::Run);
  auto r = run({"/repo", "/mnt", "-t", "gitfs.custom"});
  REQUIRE(r.action == Action::Fail);
  CHECK(r.exit_code == 1);
}

TEST_CASE("mount(8) forwarded flags are tolerated", "[options]") {
  auto r = run({"/repo", "/mnt", "-n", "-s", "-N", "ns0", "-v"});
  REQUIRE(r.action == Action::Run);
  CHECK(r.options.verbose);
}

TEST_CASE("--foreground, -f, --verbose, --help, --version", "[options]") {
  CHECK(run({"/repo", "/mnt", "--foreground"}).options.foreground);
  CHECK(run({"/repo", "/mnt", "-f"}).options.fake);
  CHECK(run({"/repo", "/mnt", "-v"}).options.verbose);
  CHECK(run({"/repo", "/mnt", "--verbose"}).options.verbose);
  CHECK(run({"--help"}).action == Action::PrintHelp);
  CHECK(run({"--version"}).action == Action::PrintVersion);
  // Short cluster forms.
  auto r = run({"/repo", "/mnt", "-vf"});
  REQUIRE(r.action == Action::Run);
  CHECK(r.options.verbose);
  CHECK(r.options.fake);
}

TEST_CASE("unknown options fail with exit 1", "[options]") {
  CHECK(run({"/repo", "/mnt", "--bogus"}).exit_code == 1);
  CHECK(run({"/repo", "/mnt", "-z"}).exit_code == 1);
  CHECK(run({"/repo", "/mnt", "-o"}).exit_code == 1);  // missing value
}

TEST_CASE("empty -o items are parameter errors", "[options]") {
  CHECK(run({"/repo", "/mnt", "-o", "rw,,ro"}).exit_code == 1);
}
