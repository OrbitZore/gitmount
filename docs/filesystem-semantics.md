# gitfs filesystem semantics

This document is the stabilized, user-facing version of the design
document's §3 ([rfc/0000-gitfs.md](../rfc/0000-gitfs.md), normative). When
this document and the RFC disagree, the RFC wins; file an issue.

## 1. Path mapping

The mount root exposes five entry directories and two synthetic files:

| Path | Contents |
|---|---|
| `/branch/<name>` | the tip commit's root tree; nested branch names (`feature/x`) render as directories |
| `/tag/<name>` | the tag's target commit (annotated tags are peeled); lightweight tags pointing at blobs/trees are `ENOENT` |
| `/commit/<full-oid>` | that exact commit's tree — full lowercase hex oid only (40 or 64 chars), no prefixes, no peeling; the directory itself is never listable |
| `/remote/<remote>/<branch>` | remote-tracking refs; the remote name is split at the first component, the rest is the branch path |
| `/HEAD` | current HEAD's snapshot; unborn HEAD (empty repository) is `ENOENT` |
| `/commits` | newline-separated oids of every commit reachable from all refs plus HEAD |
| `/.gitfs.json` | mount-time metadata snapshot (immutable for the mount) |

Rules worth memorizing:

- **Refs resolve by longest name.** In repositories whose `packed-refs` was
  edited by hand to violate the D/F rule (`refs/tags/foo` and
  `refs/tags/foo/bar` coexisting), `/tag/foo/bar` is the **ref** `foo/bar`,
  not entry `bar` of ref `foo`'s tree. A directory whose name folds a tree
  entry and a sub-ref renders as the sub-ref (directory).
- **Non-commit ref targets are invisible.** A branch/tag/remote ref or HEAD
  pointing directly at a blob or tree (possible via `git update-ref`) is
  `ENOENT` at its entry, with a warning in verbose logs. The `commits`
  list simply skips such refs.
- **`refs/remotes/<remote>/HEAD`** (symbolic form) is hidden: not listed,
  access yields `ENOENT`. A non-symbolic `refs/remotes/<remote>/HEAD`
  (direct `update-ref` write) is an ordinary tracking ref.
- **A bare namespace ref** (`refs/remotes/legacy` with nothing below it)
  renders as an empty namespace directory; its commit snapshot is not
  reachable through `/remote`. If sub-refs coexist (hand-edited
  packed-refs), the namespace becomes a merged node and the bare ref is
  reachable again.
- **`refs/replace` is never followed** — output matches
  `git --no-replace-objects`. Replace refs still count as refs for the
  `commits` list (their targets enter the walk).
- **Empty repository:** the mount succeeds; `/branch`, `/tag`, `/remote`
  are empty directories, `/HEAD` is `ENOENT`, `commits` is empty, and
  `.gitfs.json` reports `head: null`.
- **Live updates:** entry directories enumerate the refdb on every
  `opendir`; new branches/tags/remotes appear without remounting. Already
  resolved objects stay accessible while they exist in the ODB.
- **Tree entry names are raw bytes.** Non-UTF-8 names pass through
  unchanged (as `git checkout` would produce). Hand-crafted trees may
  contain entries named `.` or `..` — those are skipped with a warning.
  Names longer than `NAME_MAX` are skipped with a warning.

## 2. readdir ordering (pinned)

Every directory — root, entry dirs, grouping dirs, tree dirs and merged
nodes — lists entries in **raw byte (memcmp) dictionary order of each name
component**. Tree entries and folded sub-ref names are interleaved in one
sort, not grouped by origin. This deliberately differs from git's internal
tree ordering (which sorts directories as if suffixed with `/`), so
`LC_ALL=C sort` is the comparison oracle, not `git ls-tree` order.

## 3. Metadata

| git mode | stat result |
|---|---|
| `040000` (tree) | `S_IFDIR \| 0755` |
| `100644` (blob) | `S_IFREG \| 0644` |
| `100755` (blob) | `S_IFREG \| 0755` |
| `120000` (symlink) | `S_IFLNK \| 0777` |
| `160000` (submodule) | empty directory `0755` plus a sibling `<name>.gitfs-submodule` file |

- **Times:** file/tree timestamps are the owning commit's **committer
  time**; atime equals mtime (read-only snapshot semantics). The root,
  entry and grouping directories carry the **mount time**.
- **Submodule markers** are two-line `key=value` files (`url=`,
  `commit=<full oid>`), mode `0644`; the URL comes from that commit's
  `.gitmodules` (empty with a warning when missing). A real tree entry
  with the marker's name wins over the synthetic file.
- **Symlinks:** `readlink` returns the blob truncated at the first NUL
  (empty blob → empty target); `st_size` equals that truncated length. A
  hand-crafted symlink whose truncated length is ≥ `PATH_MAX` fails
  `readlink` with `ENAMETOOLONG` while `st_size` still reports the
  truncated length.
- **`st_ino`** is a stable 64-bit hash of the full VFS path — deliberately
  not object-derived, so identical blobs under two refs are not mistaken
  for hard links by `tar`/`rsync -H`. Inode equality never implies content
  identity. Collisions disambiguate deterministically within a mount.
- **`st_blocks`** is `ceil(st_size/512)` for regular and synthetic files
  (`du` works); directories report size 4096 and `nlink` 2.
- **`statfs`** reports the local ODB usage (packfiles + loose objects;
  alternates excluded) as `f_blocks` with 4 KiB blocks and **zero** free
  space — read-only volumes have no writable space, and `df` shows 100%
  used on purpose.

## 4. `/commits`

- Generated **on first open**, pinned to that open file description until
  close (a concurrent ref change mid-read cannot tear the buffer).
- Set = every commit reachable from **all refs plus HEAD**, in a
  deterministic order (topological walk with byte-sorted ref push order,
  HEAD last) — byte-reproducible across mounts and machines.
- `stat` before the first open reports size 0 and never triggers the walk.
- Ref targets that do not peel to commits are skipped (verbose log), never
  an error. If the walk fails because an external gc deleted objects
  mid-flight, that open returns `EIO` and the next open retries from
  scratch.

## 5. gc / prune during a mount

gitfs takes no locks and never blocks external git operations. If an
external `git gc` / `git prune` / `git update-ref` runs while mounted:

- Ref updates are picked up by the next enumeration (see live updates).
- A request that needs an object deleted or repacked away fails
  transiently with `ENOENT` (object gone) or `EIO` (pack invalidated) and
  recovers when gc finishes.
- Long-mounted repositories should set `git config gc.auto 0` or accept
  the transient errors.

## 6. st_ino registry memory bound

The path→inode registry registers lazily (only paths actually resolved by
`getattr`/`readdir`) and never recycles an inode within a mount, so
`find -inum`-style tools never see a reused number. Budget roughly
(path length + a few dozen bytes) per distinct touched path — for a
million touched paths that is on the order of tens of MB. A remount starts
a fresh registry (inode numbers are recomputed deterministically from path
hashes).

## 7. Caching and performance

- Blobs are cached in an LRU bounded by bytes (`--blob-cache-size`,
  default 64 MiB). A blob at or above the limit is never cached; opening
  it decompresses once (outside the global lock) and pins the bytes on the
  open file description.
- Every full blob decompression logs one verbose line — the observable
  hook for the "decompress exactly once per open" guarantee.
- Trees and commits use libgit2's object cache with a 1 MiB per-type
  limit and a total budget of `--tree-cache-size` (default 256 MiB).
- Metadata timeouts are 0 (`attr_timeout=0`, `entry_timeout=0`): a moved
  or deleted ref is visible immediately. `-o kernel_cache` enables page
  cache reuse across opens (safe for immutable blobs) without affecting
  metadata freshness.

## 8. Read-only enforcement

All write syscalls (`mknod`, `mkdir`, `unlink`, `rmdir`, `symlink`,
`rename`, `link`, `chmod`, `chown`, `truncate`, `utimens`, `create`,
`write`) return `EROFS`; xattr operations are not implemented
(`ENOTSUP`); the mount baseline is `ro,nosuid,nodev,default_permissions`
with the FUSE subtype `gitfs`.
