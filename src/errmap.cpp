// gitfs — read-only git-to-FUSE filesystem (RFC 0000).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "errmap.hpp"

#include <cerrno>

#include <git2.h>

namespace gitfs {

int git_to_errno(int rc) {
  if (rc >= 0) return 0;
  switch (rc) {
    case GIT_ENOTFOUND:
      return ENOENT;
    case GIT_EEXISTS:
      return EEXIST;
    case GIT_EAMBIGUOUS:
      return ENOENT;
    case GIT_EBUFS:
      return EINVAL;
    case GIT_EINVALIDSPEC:
      return ENOENT;
    case GIT_EUNBORNBRANCH:
      return ENOENT;
    case GIT_EUNCOMMITTED:
      return EINVAL;
    case GIT_EDIRECTORY:
      return EISDIR;
    case GIT_EINVALID:
      return EINVAL;
    case GIT_EPEEL:
      return EINVAL;
    default:
      // Corrupt ODB, backend read failures, and anything unexpected on the
      // data plane is an I/O error (RFC 0000 §3.3).
      return EIO;
  }
}

}  // namespace gitfs
