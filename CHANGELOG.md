# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed
- Critical Tier-2 regression (0.0.2): the large-tree binary search
  compared the probe against a plain `'\0'` terminator instead of the
  probe's directory slot (`name + '/'`), flipping the search direction
  whenever a sibling extends the name with a byte below `'/'` (hyphens:
  a file `llvm-as-fuzzer` sorts before the directory `llvm-as/`). On
  llvm-project 44 directories — 897 entries, 0.49% — became ENOENT
  while still being listed, making their whole subtrees unreachable.
  The comparator now tie-breaks against the directory slot; the
  exhaustive (N, position) disagreement-sibling matrix (832 previously
  failing combinations) is a unit test.

## [0.0.2] - 2026-09-25

### Changed
- Tier-2 metadata caches: gitmount now caches trees as raw serialized
  bytes plus a 4-byte-per-entry offset index (zero-copy entry names,
  git-order binary search) and commits as compact facts tuples (root
  tree + committer time, tag chains resolved on demand), under one
  byte-exact `--tree-cache-size` budget. libgit2's parsed-object cache
  (accounted by serialized size, resident at 1.4-1.7x for trees and
  more for commits) is fully disabled. Measured: the metadata budget
  now holds within ~5% of its configured value; lookups stay within
  the §3.5 performance guardrails (grep +4.5%, find +12% on the
  synthetic walk benchmark).
### Fixed
- A mountpoint that is a regular file is now rejected up front with a
clear error (exit 1, also under `-f`); previously libfuse accepted the
  mount and every access returned EIO (stress-test finding ①).
- Blob cache miss loads no longer copy the payload a second time on
  insert (finding ②, transient double buffering).
- glibc's dynamic mmap threshold is pinned at startup: multi-megabyte
  payloads now come from mmap and are returned to the OS on release,
  eliminating arena retention that grew ~3x the configured blob cache
  under concurrent churn (finding ②; anonymous memory now ≈ 1.1× the
  configured cache on the churn workload).

### Changed
- Documentation now states the real memory semantics: cache knobs bound
  accounted payload bytes; RSS additionally contains parsed tree/commit
  metadata (~4–6× the tree budget on large histories), bookkeeping and
  reclaimable mmap'd packfile pages (finding ②), and the man page no
  longer claims non-commit tags are hidden from the `tag/` listing —
  listings are enumeration hints; such tags return ENOENT on access
  (finding ③).

## [0.0.1] - 2026-09-25

First packaged release.

### Changed
- Renamed the project from **gitfs** to **gitmount** (in place, per the
  frozen RFC 0000): the mount helper is `mount.gitmount`, the fstype is
  `gitmount`, and the synthetic files are `.gitmount.json` /
  `.gitmount-submodule`. The earlier name collided with
  [presslabs/gitfs](https://github.com/presslabs/gitfs) — a different,
  older project.

### Added
- Core filesystem per RFC 0000 §3: `/branch`, `/tag`, `/commit`,
  `/remote`, `/HEAD` namespaces with longest-ref-name resolution, merged
  nodes (packed-refs D/F violations), hidden symbolic remote HEADs, live
  ref enumeration, and `refs/replace` passthrough.
- `/commits` — first-open generation (single-flight, chunked revwalk,
  fingerprint invalidation, deterministic topological order) and
  `.gitmount.json` mount metadata.
- Metadata mapping per RFC 0000 §3.2: mode table, committer-time
  timestamps, path-hash `st_ino` with collision disambiguation registry,
  submodule markers, symlink truncation semantics.
- Blob LRU cache with segmented (lock-release) loading, oversized-blob
  open-pin decompression outside the global lock, and per-decompression
  verbose logging; libgit2 cache tuning (tree/commit 1 MiB, blob pinned
  to 0).
- mount(8) helper CLI per RFC 0000 §3.7: option permutation, double-track
  `-o` triage, `-f` fake validation, exit codes 0–3.
- Engineering scaffolding: CMake build, Catch2 v3 unit tests, generated
  fixture repository, real-mount integration suite, CI matrix, man page,
  GPL-3.0-or-later licensing.

[Unreleased]: https://github.com/OrbitZore/gitmount/compare/v0.0.2...HEAD
[0.0.2]: https://github.com/OrbitZore/gitmount/compare/v0.0.1...v0.0.2
[0.0.1]: https://github.com/OrbitZore/gitmount/releases/tag/v0.0.1
