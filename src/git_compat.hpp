// gitmount — read-only git-to-FUSE filesystem (RFC 0000).
// SPDX-FileCopyrightText: 2026 The gitmount authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unified libgit2 include point (issue #8): every translation unit that
// needs git2 headers includes this file instead of <git2.h> directly, so
// the compile-time guard below is enforced for the whole program — not
// just the TUs that happen to include <git2.h> themselves.
#pragma once

#include <git2.h>

#ifdef GIT_OID_SHA256_HEXSIZE
// 该宏仅在 libgit2 以 GIT_EXPERIMENTAL_SHA256 构建的头文件分支中定义。
#error \
    "gitmount v0.1 does not support libgit2 built with GIT_EXPERIMENTAL_SHA256: the git_oid API/ABI differs (3-argument git_oid_fromstr, larger git_oid). See RFC 0000 §1.3."
#endif
