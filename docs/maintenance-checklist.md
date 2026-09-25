# gitmount maintenance checklist

Operational checklists for changes that need coordinated updates across
code, docs and tests. Sources of truth: [rfc/0000-gitmount.md](../rfc/0000-gitmount.md)
(normative design), [mount.gitmount.8](mount.gitmount.8) (man page),
[src/options.cpp](../src/options.cpp) (option triage tables).

## New util-linux release (RFC 0000 §3.7 evolution clause)

mount(8) forwards unfiltered `-o` keys to helpers. When util-linux changes
what reaches `mount.gitmount`:

1. **Diff the man page** — compare the "Filesystem-independent mount
   options" section of `man mount(8)` against the previous release; note
   added/removed keys.
2. **Diff the forwarding set** — check `libmount`'s `exec_helper` /
   option-filter list (`libmount/src/optlist.c`, `mount.c` in the
   util-linux tree) for newly filtered or newly forwarded keys.
3. **Triage each new arriving key**: is it a no-op for gitmount?
   - no-op → add to the **enumerate track** in `src/options.cpp`
     (`kEnumerateTrack`) and to the unit test list in
     `tests/unit/test_options.cpp`.
   - semantic/security impact → add to the **reject track**
     (`kRejectTrack`) with a dedicated error message.
   - ambiguous → open an issue; until resolved the key falls through to
     libfuse (unknown → exit 1), which is the safe default.
4. **Update docs**: `docs/mount.gitmount.8` (INVOCATION/OPTIONS),
   `README.md` if user-visible, and this checklist's snapshot below.
5. **Run** `ctest --test-dir build` — the integration suite has exec-path
   scenarios gated on root; re-run locally with sudo when the forwarding
   set changes.

Current snapshot: verified against util-linux 2.42.3 (measured argv
capture, see RFC 0000 §3.7). Known quirks preserved in tests: options
arrive after the positional arguments; `rw` is pre-seeded unless the
caller asked for `ro`; `user`/`users` implicitly carry
`noexec,nosuid,nodev`; `owner`/`group` carry `nosuid,nodev`.

## Man page version bump

`docs/mount.gitmount.8` carries the reviewed-decision watermark in its first
comment line. Whenever user-visible semantics change, update the page and
bump the watermark (RFC 0000 Q31 process convention).

## libgit2 upgrade

- Re-run `tests/unit` + the full integration suite (fixture includes
  hand-crafted trees, packed-refs D/F violations, pathological symlinks —
  they catch parser regressions).
- `refs/replace` remains unimplemented upstream (libgit2 ≤ 1.9); if a
  future version adds replace following, **disable it** and add a unit
  test asserting the passthrough semantics (RFC 0000 §3.1).
- `libgit2_configure_cache` pins libgit2's parsed-object cache fully
  off (blob/tree/commit per-type limits 0) — gitmount's Tier-2 metadata
  cache (`src/rawobj.*`, `MetaLruCache`) parses raw bytes itself.
  If a future libgit2 changes raw object formats (it must not — the
  object format is git's, not libgit2's), the raw parsers' unit tests
  will catch it.

## FUSE / kernel changes

- The `use_ino` option was removed in libfuse3 (high-level mounts always
  honor the filesystem's `st_ino`); gitmount keeps accepting `use_ino` in
  `-o` as a redundant baseline synonym without forwarding it. If libfuse
  ever reintroduces a switch, re-evaluate.
- readdirplus: gitmount fills plain `dirent` stats (ino + type) only; do not
  claim `FUSE_FILL_DIR_PLUS` without full attribute support.

## Performance gates (RFC 0000 §3.5)

On self-hosted runners (shared-runner walls are too noisy):

- `grep -r` over a full tree ≥ 50% of `git archive | tar -x` throughput —
  measure on content-heavy repositories (large blobs); tiny-file
  workloads are latency-bound by the pinned `attr_timeout=0` /
  `entry_timeout=0` (correctness first: refs are mutable).
- Cold-cache `cat` of a single file ≤ 1.5× the `git cat-file blob` base.
- Oversized-blob sequential read: verbose decompression counter must be
  exactly 1 per open (asserted in the integration suite — keep it green).

Measured baselines (dev machine, 5000-file synthetic tree, warm caches):

| Workload | ext4 | gitmount | Notes |
|---|---|---|---|
| `find -type f` (metadata walk) | 6 ms | 49 ms | ~10 µs/file; libgit2 tree cache effective |
| `stat` of a root-level entry | — | ~35 µs | ≈ 2 FUSE round trips |
| `stat` of a 4-deep path | — | ~207 µs | trivial-fs baseline with the same mount options: ~90 µs; the delta is per-LOOKUP re-resolution + refdb probing (RFC 0000 §3.5 anticipated O(depth) re-resolution) |
| open+read+close (4-deep) | 5 µs | ~351 µs | round-trip count dominated; `entry_timeout=0` forces a full re-walk per syscall |
| direct blob reads (libgit2, no FUSE) | — | 4.1 µs/blob | data plane itself is not the bottleneck |

Optimization headroom (v0.1 deliberately keeps the simple pinned design):
the longest-ref-match loop probes the refdb (loose + packed) for several
prefixes per component; a per-namespace ref-name memo would cut handler
latency if the M3 gates miss on metadata-heavy loads.
