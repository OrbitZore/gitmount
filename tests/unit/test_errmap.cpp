// gitmount unit tests — git_to_errno (RFC 0000 §3.3).
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <cerrno>

#include "errmap.hpp"

#include <git2.h>

using gitmount::git_to_errno;

TEST_CASE("success maps to zero", "[errmap]") {
  CHECK(git_to_errno(0) == 0);
  CHECK(git_to_errno(1) == 0);
  CHECK(git_to_errno(GIT_OK) == 0);
}

TEST_CASE("not-found family maps to ENOENT", "[errmap]") {
  CHECK(git_to_errno(GIT_ENOTFOUND) == ENOENT);
  CHECK(git_to_errno(GIT_EUNBORNBRANCH) == ENOENT);
  CHECK(git_to_errno(GIT_EAMBIGUOUS) == ENOENT);
  CHECK(git_to_errno(GIT_EINVALIDSPEC) == ENOENT);
}

TEST_CASE("type/peel confusion maps to EINVAL", "[errmap]") {
  CHECK(git_to_errno(GIT_EINVALID) == EINVAL);
  CHECK(git_to_errno(GIT_EPEEL) == EINVAL);
  CHECK(git_to_errno(GIT_EBUFS) == EINVAL);
}

TEST_CASE("unknown negative codes map to EIO", "[errmap]") {
  CHECK(git_to_errno(-1) == EIO);   // generic error: corrupt ODB, read fail
  CHECK(git_to_errno(-99) == EIO);  // unmapped code
}

TEST_CASE("exists maps to EEXIST", "[errmap]") { CHECK(git_to_errno(GIT_EEXISTS) == EEXIST); }
