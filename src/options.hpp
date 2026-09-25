// gitmount — read-only git-to-FUSE filesystem (RFC 0000).
// SPDX-FileCopyrightText: 2026 The gitmount authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// CLI / mount(8) helper argument parsing (RFC 0000 §3.7).
//
// Helper contract: [-sfnv] [-N namespace] [-o options] [-t type.subtype]
//                  <src> <dir>
// mount(8) places the options AFTER the positional arguments (measured
// argv: "src dir -f -o rw"), so the parser tolerates GNU getopt-style
// permutation. Direct calls may use the same forms.
//
// The -o string is triaged per RFC 0000 §3.7 (double-track):
//   own keys          blob-cache-size= / tree-cache-size= (hyphen and
//                     underscore spellings equivalent), later occurrence
//                     wins; values are positive decimal MiB (0 / negative /
//                     non-numeric / overflow -> parameter error)
//   baseline synonyms ro / nosuid / nodev / default_permissions / use_ino
//                     accepted as redundant no-ops
//   rw                accepted as a no-op with a stderr warning (libmount
//                     pre-seeds it unconditionally; ntfs-3g precedent)
//   fsname=           may override the cosmetic baseline fsname
//   subtype=          rejected: the baseline subtype=gitmount is protected
//                     (/proc/mounts type field, mount -t gitmount matching)
//   enumerate track   measured no-op VFS keys -> accept, ignore, verbose
//   reject track      suid dev remount uid= gid= umask= context= fscontext=
//                     defcontext= rootcontext= -> dedicated error, exit 1
//   table track       man mount(8) "Filesystem-independent" no-op keys
//                     (noiversion norelatime nostrictatime nolazytime) ->
//                     accept, ignore, verbose
//   everything else   passed through to libfuse's option parser (unknown
//                     keys are rejected by libfuse -> exit 1)
//
// Keys are matched on the part before the first '=' (RFC 0000 §3.7 rule 1:
// user=alice, nofail=1, ro=vfs arrive with values; the key name decides).
// Comma splitting never re-splits values on ':' (context= values contain
// colons and arrive whole).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gitmount::cli {

struct Options {
  std::string repository;
  std::string mountpoint;

  bool foreground = false;
  bool verbose = false;
  bool fake = false;  // -f: full validation incl. repo readability, no mount

  std::uint64_t blob_cache_bytes = 64ULL << 20;   // 64 MiB default
  std::uint64_t tree_cache_bytes = 256ULL << 20;  // 256 MiB default
  std::string fsname = "gitmount";

  // Keys passed through to libfuse (e.g. kernel_cache, allow_other).
  std::vector<std::string> fuse_passthrough;

  // Diagnostics emitted while parsing (rw warning, ignored-key notices);
  // recorded for unit tests, also printed by the parser.
  std::vector<std::string> warnings;
};

enum class Action {
  Run,           // options valid; caller should proceed
  PrintHelp,     // --help
  PrintVersion,  // --version
  Fail,          // parameter error; exit_code carries the code (always 1)
};

struct ParseResult {
  Action action = Action::Run;
  int exit_code = 0;
  std::string message;  // Fail: human-readable error for stderr
  Options options;      // valid when action == Run
};

ParseResult parse(int argc, char* const argv[]);

const char* version_string();
const char* usage_text();

}  // namespace gitmount::cli
