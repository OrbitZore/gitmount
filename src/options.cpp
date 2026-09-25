// gitfs — read-only git-to-FUSE filesystem (RFC 0000).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "options.hpp"

#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "log.hpp"

namespace gitfs::cli {

namespace {

// ---------------------------------------------------------------------------
// Track tables (RFC 0000 §3.7).
// ---------------------------------------------------------------------------

// Enumerate track: measured util-linux 2.42.3 keys that reach the -o string
// and are no-ops for gitfs. Sourced from real exec'd-helper argv capture;
// maintained via docs/maintenance-checklist.md when util-linux evolves.
constexpr std::array<const char*, 31> kEnumerateTrack = {
    "atime", "noatime",  "relatime", "strictatime", "lazytime",    "diratime", "nodiratime",
    "sync",  "async",    "dirsync",  "exec",        "noexec",      "user",     "users",
    "owner", "group",    "nouser",   "symfollow",   "nosymfollow", "iversion", "silent",
    "loud",  "mand",     "nomand",   "nofs",        "_netdev",     "nofail",   "acl",
    "quiet", "showexec", "bsdgroups"};

// Reject track: man mount(8) "Filesystem-independent" table keys with
// semantics or security impact for gitfs -> dedicated error, exit 1.
constexpr std::array<const char*, 10> kRejectTrack = {
    "suid",  "dev",     "remount",   "uid",        "gid",
    "umask", "context", "fscontext", "defcontext", "rootcontext"};

// Table track: man mount(8) "Filesystem-independent" table keys that are
// no-ops for gitfs and are NOT in the enumerate track (the table also backs
// negated/valued forms such as noiversion; rule 1 cuts at '=' first).
constexpr std::array<const char*, 4> kTableTrack = {"noiversion", "norelatime", "nostrictatime",
                                                    "nolazytime"};

template <typename Table>
bool in_table(const Table& table, const std::string& key) {
  for (const char* k : table)
    if (key == k) return true;
  return false;
}

// Baseline-synonym keys (RFC 0000 §3.7): accepted as redundant no-ops.
constexpr std::array<const char*, 5> kBaselineSynonyms = {"ro", "nosuid", "nodev",
                                                          "default_permissions", "use_ino"};

enum class OwnKey { None, BlobCacheSize, TreeCacheSize };

OwnKey classify_own(const std::string& key) {
  auto norm = [](std::string s) {
    for (auto& c : s)
      if (c == '_') c = '-';
    return s;
  };
  const std::string k = norm(key);
  if (k == "blob-cache-size") return OwnKey::BlobCacheSize;
  if (k == "tree-cache-size") return OwnKey::TreeCacheSize;
  return OwnKey::None;
}

// Positive decimal integer of MiB -> bytes. Rejects 0, signs, non-digits,
// empty strings, and anything that overflows uint64 (RFC 0000 §3.7).
bool parse_mib(const std::string& value, std::uint64_t* out) {
  if (value.empty() || value.size() > 19) return false;
  for (char c : value)
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  errno = 0;
  char* end = nullptr;
  unsigned long long v = std::strtoull(value.c_str(), &end, 10);
  if (errno != 0 || end == nullptr || *end != '\0' || v == 0) return false;
  constexpr unsigned long long kMax = ~0ULL;
  if (v > kMax >> 20) return false;  // v * 2^20 would overflow
  *out = v << 20;
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// -o string processing
// ---------------------------------------------------------------------------

namespace {

// Processes one comma-separated options string into `opts`.
// Returns false (and fills `error`) on a parameter error.
bool apply_options_string(const std::string& str, Options& opts, std::string& error) {
  std::size_t pos = 0;
  while (pos <= str.size()) {
    std::size_t comma = str.find(',', pos);
    std::string item =
        str.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    pos = (comma == std::string::npos) ? str.size() + 1 : comma + 1;
    if (item.empty()) {
      error = "empty option in -o list";
      return false;
    }

    // Rule 1: the key is the part before the first '=' (values like
    // user=alice, nofail=1, ro=vfs, context=system_u:object_r:...:s0 stay
    // whole — never re-split values on ':').
    std::string key = item;
    std::string value;
    std::size_t eq = item.find('=');
    if (eq != std::string::npos) {
      key = item.substr(0, eq);
      value = item.substr(eq + 1);
    }

    switch (classify_own(key)) {
      case OwnKey::BlobCacheSize: {
        std::uint64_t bytes = 0;
        if (!parse_mib(value, &bytes)) {
          error = "invalid value for blob-cache-size: '" + value +
                  "' (expected a positive integer number of MiB)";
          return false;
        }
        opts.blob_cache_bytes = bytes;  // later occurrence wins
        continue;
      }
      case OwnKey::TreeCacheSize: {
        std::uint64_t bytes = 0;
        if (!parse_mib(value, &bytes)) {
          error = "invalid value for tree-cache-size: '" + value +
                  "' (expected a positive integer number of MiB)";
          return false;
        }
        opts.tree_cache_bytes = bytes;
        continue;
      }
      case OwnKey::None:
        break;
    }

    if (key == "fsname") {
      opts.fsname = value;  // cosmetic override of the baseline
      continue;
    }
    if (key == "subtype") {
      error =
          "option 'subtype' cannot be overridden: the gitfs subtype is part "
          "of the protected mount baseline";
      return false;
    }
    if (key == "rw") {
      // libmount pre-seeds rw unconditionally for mounts without explicit
      // -r/-o ro; accept as a no-op with a stderr warning (RFC 0000 §3.7).
      opts.warnings.push_back("-o rw ignored: gitfs mounts are always read-only");
      continue;
    }
    if (in_table(kBaselineSynonyms, key)) {
      log::vlog(
          "mount option '%s' accepted as redundant (already part of "
          "the gitfs baseline)",
          item.c_str());
      continue;
    }
    if (in_table(kEnumerateTrack, key)) {
      log::vlog("mount option '%s' accepted and ignored (no-op for gitfs)", item.c_str());
      continue;
    }
    if (in_table(kRejectTrack, key)) {
      error = std::string("mount option '") + key +
              "' is not supported by gitfs (read-only filesystem: no "
              "setuid/device semantics, no remount, no uid/gid/umask "
              "mapping, no SELinux relabeling)";
      return false;
    }
    if (in_table(kTableTrack, key)) {
      log::vlog("mount option '%s' accepted and ignored (no-op for gitfs)", item.c_str());
      continue;
    }
    // Neither track claims it: pass through to libfuse (unknown keys are
    // rejected by libfuse's parser -> exit 1).
    opts.fuse_passthrough.push_back(item);
  }
  return true;
}

}  // namespace

const char* version_string() { return "gitfs 0.0.1"; }

const char* usage_text() {
  return R"(usage: mount.gitfs [options] <repository> <mountpoint>
       (options may appear after the positional arguments — mount(8)
        execs the helper as e.g. "<repo> <dir> -f -o rw")

Mount a local git repository (bare or non-bare) as a read-only FUSE
filesystem exposing branch/, tag/, commit/, remote/, HEAD/, commits
and .gitfs.json at the mount root. Equivalent forms:

  mount -t gitfs <repo> <dir>          via mount(8) exec'ing this helper
  mount <dir>                          via an /etc/fstab entry
  mount.gitfs <repo> <dir> [options]   direct invocation

options:
  -o OPT[,OPT...]               mount options (comma separated)
      blob-cache-size=<MiB>     blob LRU cache limit (default 64)
      tree-cache-size=<MiB>     libgit2 tree/commit cache budget (default 256)
                                  hyphen and underscore spellings equivalent;
                                  repeated keys: last one wins; value must be
                                  a positive integer (MiB)
      fsname=<name>             override the displayed filesystem name
      kernel_cache, allow_other,...   passed through to libfuse
      rw is accepted as a no-op with a warning (mount is always read-only);
      irrelevant VFS keys (noatime, nofail, ...) are accepted and ignored;
      suid, dev, remount, uid=, gid=, umask=, context=, fscontext=,
      defcontext=, rootcontext= and subtype= are rejected
  --blob-cache-size <MiB>       same as -o blob-cache-size=<MiB>
  --tree-cache-size <MiB>       same as -o tree-cache-size=<MiB>
  --foreground                  run in the foreground (default: daemonize);
                                  the short -f is reserved for mount(8) fake
  -f                            fake: validate arguments, options and
                                  repository readability, then exit without
                                  mounting (bad repository -> exit 2)
  --verbose, -v                 path resolution, cache hits and blob
                                  decompression events (one log line per
                                  full decompression: oversized open-pinned
                                  blobs and cache-miss loads)
  -n, -s, -N <ns>               mount(8) forwarded flags (no-mtab / sloppy /
                                  namespace): tolerated and ignored
  -t gitfs                      accepted and ignored (verbose note); any
                                  other value is a fstype mismatch (exit 1)
  --version, --help

exit codes: 0 success (or fake validation passed)
            1 parameter error
            2 repository unreadable or not a git repository
            3 mount failure)";
}

// ---------------------------------------------------------------------------
// argv parsing
// ---------------------------------------------------------------------------

ParseResult parse(int argc, char* const argvIn[]) {
  ParseResult res;
  Options& opts = res.options;

  std::vector<std::string> positionals;
  bool no_more_options = false;

  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i)  // argv[0] is the program name
    args.emplace_back(argvIn[i]);

  std::size_t i = 0;
  while (i < args.size()) {
    const std::string& arg = args[i];
    if (no_more_options || arg.empty() || arg == "-" || arg[0] != '-') {
      positionals.push_back(arg);
      ++i;
      continue;
    }
    if (arg == "--") {
      no_more_options = true;
      ++i;
      continue;
    }
    if (arg.rfind("--", 0) == 0) {
      const std::string longopt = arg.substr(2);
      if (longopt == "help") {
        res.action = Action::PrintHelp;
        return res;
      }
      if (longopt == "version") {
        res.action = Action::PrintVersion;
        return res;
      }
      if (longopt == "foreground") {
        opts.foreground = true;
        ++i;
        continue;
      }
      if (longopt == "verbose") {
        opts.verbose = true;
        ++i;
        continue;
      }
      if (longopt.rfind("blob-cache-size", 0) == 0 || longopt.rfind("tree-cache-size", 0) == 0) {
        // Both "--opt <value>" and "--opt=<value>" forms are accepted.
        const std::size_t eq = longopt.find('=');
        const std::string name = longopt.substr(0, eq == std::string::npos ? longopt.size() : eq);
        std::string value;
        if (eq != std::string::npos) {
          value = longopt.substr(eq + 1);
        } else {
          if (i + 1 >= args.size()) {
            res.action = Action::Fail;
            res.exit_code = 1;
            res.message = "option --" + name + " requires a value (MiB)";
            return res;
          }
          value = args[++i];
        }
        std::uint64_t bytes = 0;
        if (!parse_mib(value, &bytes)) {
          res.action = Action::Fail;
          res.exit_code = 1;
          res.message = "invalid value for --" + name + ": '" + value +
                        "' (expected a positive integer number of MiB)";
          return res;
        }
        if (name == "blob-cache-size")
          opts.blob_cache_bytes = bytes;
        else
          opts.tree_cache_bytes = bytes;
        ++i;
        continue;
      }
      res.action = Action::Fail;
      res.exit_code = 1;
      res.message = "unknown option: " + arg;
      return res;
    }

    // Short option cluster, e.g. "-vf" or "-o rw".
    for (std::size_t c = 1; c < arg.size(); ++c) {
      const char opt = arg[c];
      switch (opt) {
        case 'v':
          opts.verbose = true;
          break;
        case 'f':
          opts.fake = true;
          break;
        case 'n':
        case 's':
          break;  // tolerated, ignored (mount(8) contract)
        case 'o':
        case 'N':
        case 't': {
          std::string value;
          if (c + 1 < arg.size()) {
            value = arg.substr(c + 1);
          } else {
            if (i + 1 >= args.size()) {
              res.action = Action::Fail;
              res.exit_code = 1;
              res.message = std::string("option -") + opt + " requires a value";
              return res;
            }
            value = args[++i];
          }
          if (opt == 'o') {
            std::string error;
            if (!apply_options_string(value, opts, error)) {
              res.action = Action::Fail;
              res.exit_code = 1;
              res.message = error;
              return res;
            }
          } else if (opt == 't') {
            if (value == "gitfs") {
              log::vlog("-t gitfs accepted (redundant self-reference)");
            } else {
              res.action = Action::Fail;
              res.exit_code = 1;
              res.message = "fstype mismatch: -t '" + value + "' but this helper mounts 'gitfs'";
              return res;
            }
          }
          // 'N': namespace flag forwarded by mount(8) — tolerated, ignored.
          c = arg.size();  // value consumed the rest of the cluster
          break;
        }
        default:
          res.action = Action::Fail;
          res.exit_code = 1;
          res.message = std::string("unknown option: -") + opt;
          return res;
      }
    }
    ++i;
  }

  if (positionals.size() != 2) {
    res.action = Action::Fail;
    res.exit_code = 1;
    if (positionals.size() < 2) {
      res.message = positionals.empty() ? "missing arguments: expected <repository> <mountpoint>"
                                        : "missing argument: expected <repository> <mountpoint>";
    } else {
      res.message = "too many arguments (expected <repository> <mountpoint>)";
    }
    return res;
  }
  opts.repository = positionals[0];
  opts.mountpoint = positionals[1];

  for (const auto& w : opts.warnings) log::warn("%s", w.c_str());
  res.action = Action::Run;
  return res;
}

}  // namespace gitfs::cli
