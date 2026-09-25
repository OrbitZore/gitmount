# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Core filesystem per RFC 0000 §3: `/branch`, `/tag`, `/commit`,
  `/remote`, `/HEAD` namespaces with longest-ref-name resolution, merged
  nodes (packed-refs D/F violations), hidden symbolic remote HEADs, live
  ref enumeration, and `refs/replace` passthrough.
- `/commits` — first-open generation (single-flight, chunked revwalk,
  fingerprint invalidation, deterministic topological order) and
  `.gitfs.json` mount metadata.
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

[Unreleased]: https://github.com/OrbitZore/gitfs/commits/main/CHANGELOG.md
