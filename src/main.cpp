// gitmount — read-only git-to-FUSE filesystem (RFC 0000).
// SPDX-FileCopyrightText: 2026 The gitmount authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mount(8) helper entry point (RFC 0000 §3.7). Exit codes:
//   0  mount success (parent exits after daemonizing), graceful foreground
//      termination, or fake validation passed
//   1  parameter error (including libfuse rejection of unknown -o keys)
//   2  repository unreadable / not a git repository
//   3  mount failure
#include <limits.h>
#include <malloc.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>

#include "gitmount.hpp"
#include "gitrepo.hpp"
#include "log.hpp"
#include "options.hpp"

namespace {

// Canonicalize the repository path with realpath(3) — no symlink-based
// TOCTOU between validation and mount (RFC 0000 §4).
bool canonicalize(const std::string& in, std::string* out) {
  char* resolved = ::realpath(in.c_str(), nullptr);
  if (!resolved) return false;
  *out = resolved;
  ::free(resolved);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace gitmount;

  const auto parsed = cli::parse(argc, argv);
  if (parsed.action == cli::Action::PrintHelp) {
    std::fputs(cli::usage_text(), stdout);
    return 0;
  }
  if (parsed.action == cli::Action::PrintVersion) {
    std::puts(cli::version_string());
    return 0;
  }
  if (parsed.action == cli::Action::Fail) {
    std::fprintf(stderr, "mount.gitmount: %s\n", parsed.message.c_str());
    std::fputs("Try 'mount.gitmount --help' for more information.\n", stderr);
    return parsed.exit_code;
  }

  const cli::Options& opts = parsed.options;
  log::set_verbose(opts.verbose);

#if defined(__GLIBC__)
  // Allocator tuning (stress-test finding ②): glibc's dynamic mmap
  // threshold grows every time a large block is freed, after which
  // same-sized allocations come from the brk heap and are NOT returned
  // to the OS on free. A cache that repeatedly loads and evicts
  // multi-MB blobs across many threads makes per-thread arenas balloon
  // deterministically (measured: ~14x the configured blob cache on a
  // 183k-file repository). Pinning the threshold (any explicit
  // mallopt(M_MMAP_THRESHOLD) disables the dynamic adjustment) keeps
  // large payloads on mmap and returned on release.
  ::mallopt(M_MMAP_THRESHOLD, 1 << 20);
  ::mallopt(M_TRIM_THRESHOLD, 2 << 20);
#endif

  // Mountpoint sanity check (stress-test finding ①): libfuse happily
  // mounts over a regular file and every later access returns EIO, so
  // reject it up front — also under -f, where predicting boot-time
  // failure is the whole point of fake validation. A nonexistent
  // mountpoint stays a mount-time failure (exit 3).
  struct stat mp_st {};
  if (::stat(opts.mountpoint.c_str(), &mp_st) == 0 && !S_ISDIR(mp_st.st_mode)) {
    std::fprintf(stderr, "mount.gitmount: mountpoint '%s' is not a directory\n",
                 opts.mountpoint.c_str());
    return 1;
  }

  libgit2_global_init();
  libgit2_configure_cache(opts.tree_cache_bytes);

  // ---- repository validation (exit code 2 path, RFC 0000 §3.7) ----------
  std::string abs_repo;
  if (!canonicalize(opts.repository, &abs_repo)) {
    std::fprintf(stderr, "mount.gitmount: cannot access repository '%s': %s\n",
                 opts.repository.c_str(), std::strerror(errno));
    libgit2_global_shutdown();
    return 2;
  }
  std::string git_err;
  auto repo = GitRepo::open(abs_repo, &git_err);
  if (!repo) {
    std::fprintf(stderr, "mount.gitmount: '%s' is not a readable git repository: %s\n",
                 opts.repository.c_str(), git_err.c_str());
    libgit2_global_shutdown();
    return 2;
  }

  auto fs = std::make_unique<Gitmount>(std::move(repo), abs_repo, opts.blob_cache_bytes,
                                       opts.tree_cache_bytes);

  // ---- FUSE argument assembly -------------------------------------------
  // Hardcoded baseline (RFC 0000 §3.7): ro,fsname,default_permissions,
  // subtype,nosuid,nodev — plus attr_timeout=0/entry_timeout=0 so ref moves
  // are never masked by a staleness window (§3.5). fsname may be overridden
  // cosmetically; user keys no track claimed (e.g. kernel_cache) are
  // appended for libfuse to validate.
  //
  // Note on use_ino: libfuse3 removed the option — high-level mounts always
  // honor the st_ino the filesystem fills in (libfuse 3.0 changelog), which
  // is exactly the RFC's intent. "use_ino" stays accepted in -o parsing as
  // a redundant baseline synonym; it is deliberately not forwarded.
  std::vector<std::string> arg_storage = {
      "mount.gitmount",
      "-o",
      "ro,fsname=" + opts.fsname + ",default_permissions,subtype=gitmount,nosuid,nodev",
      "-o",
      "attr_timeout=0",
      "-o",
      "entry_timeout=0"};
  for (const auto& extra : opts.fuse_passthrough) arg_storage.push_back(extra);

  struct fuse_args fargs = FUSE_ARGS_INIT(0, nullptr);
  for (const auto& a : arg_storage) fuse_opt_add_arg(&fargs, a.c_str());

  auto teardown_args = [&fargs] { fuse_opt_free_args(&fargs); };

  struct fuse* fuse = fuse_new(&fargs, Gitmount::fuse_ops(), sizeof(fuse_operations), fs.get());
  if (!fuse) {
    // libfuse rejected the options (unknown passthrough key) — parameter
    // error per RFC 0000 §3.7.
    std::fprintf(stderr,
                 "mount.gitmount: invalid mount options (rejected by "
                 "libfuse)\n");
    teardown_args();
    return 1;
  }

  // Fake mode (-f): full parameter, option and repository validation, no
  // mount and no daemonization (RFC 0000 §3.7). fuse_new already validated
  // every option above.
  if (opts.fake) {
    fuse_destroy(fuse);
    teardown_args();
    return 0;
  }

  if (fuse_mount(fuse, opts.mountpoint.c_str()) != 0) {
    std::fprintf(stderr, "mount.gitmount: cannot mount at '%s': %s\n", opts.mountpoint.c_str(),
                 std::strerror(errno));
    fuse_destroy(fuse);
    teardown_args();
    return 3;
  }

  // Daemonize after a successful mount (exit 0 is reported by the parent —
  // RFC 0000 §3.7), then install the graceful signal handlers (SIGINT/
  // SIGTERM -> session exit, RFC 0000 §3.4).
  fuse_daemonize(opts.foreground ? 1 : 0);
  if (fuse_set_signal_handlers(fuse_get_session(fuse)) != 0) {
    std::fprintf(stderr, "mount.gitmount: cannot install signal handlers\n");
    fuse_unmount(fuse);
    fuse_destroy(fuse);
    teardown_args();
    return 3;
  }

  log::vlog("mounted %s at %s (blob cache %llu MiB, tree cache %llu MiB)", abs_repo.c_str(),
            opts.mountpoint.c_str(), static_cast<unsigned long long>(opts.blob_cache_bytes >> 20),
            static_cast<unsigned long long>(opts.tree_cache_bytes >> 20));

  // FUSE_USE_VERSION 31 keeps the portable int-clone_fd form, working
  // across every fuse3 release (≥ 3.10) that RFC 0000 §1.2 targets.
  const int loop_rc = fuse_loop_mt(fuse, 1 /* clone_fd */);

  fuse_remove_signal_handlers(fuse_get_session(fuse));
  fuse_unmount(fuse);
  fuse_destroy(fuse);
  teardown_args();

  libgit2_global_shutdown();
  return loop_rc == 0 ? 0 : 3;
}
