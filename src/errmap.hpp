// gitfs — read-only git-to-FUSE filesystem (RFC 0000).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace gitfs {

// Translate a libgit2 error code to an errno value (RFC 0000 §3.3).
//
// Pinned semantics:
//   0                    -> 0
//   GIT_ENOTFOUND        -> ENOENT   (object/ref does not exist; invalid oid
//                          format is normalized to ENOENT so internal rules
//                          are not leaked to callers)
//   GIT_EUNBORNBRANCH    -> ENOENT   (unborn HEAD)
//   GIT_EAMBIGUOUS       -> ENOENT
//   GIT_EINVALIDSPEC     -> ENOENT
//   GIT_EINVALIDTYPE /   -> EINVAL   (type confusion on a resolved object;
//      GIT_EPEEL                      peel failure is handled by callers)
//   anything else < 0    -> EIO      (ODB corruption, read errors, ...)
//
// Non-error degradations (readlink NUL truncation, empty symlink target)
// are handled in gitfs.cpp and must never reach this table (§3.3 note).
int git_to_errno(int git_error_code);

}  // namespace gitfs
