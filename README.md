<div align="center">

# gitmount

**Mount a git repository as a read-only filesystem.**

Browse every branch, tag and commit with plain `ls`, `cat`, `grep` and
`diff` — no checkout, no worktree, no archive.

[![CI](https://github.com/OrbitZore/gitmount/actions/workflows/ci.yml/badge.svg)](https://github.com/OrbitZore/gitmount/actions/workflows/ci.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPL%20v3-blue.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus)
![Platform](https://img.shields.io/badge/platform-Linux-fcc624?logo=linux)
![libgit2](https://img.shields.io/badge/libgit2-%E2%89%A51.4-ef3a24?logo=git)
![FUSE 3](https://img.shields.io/badge/FUSE-3-00599C)

**English** · [简体中文](README.zh-CN.md)

</div>

---

```console
$ sudo mount -t gitmount /srv/repos/linux.git /mnt/linux
$ ls -A /mnt/linux
.gitmount.json  HEAD  branch  commit  commits  remote  tag
$ ls /mnt/linux/tag
v5.4  v6.1  v6.6  v6.12  v6.13
$ grep -n '^VERSION\|^PATCHLEVEL' /mnt/linux/tag/v6.13/Makefile
1:VERSION = 6
2:PATCHLEVEL = 13
$ diff -r --brief /mnt/linux/tag/v6.12 /mnt/linux/tag/v6.13 | head -3
Files /mnt/linux/tag/v6.12/Makefile and /mnt/linux/tag/v6.13/Makefile differ
Only in /mnt/linux/tag/v6.13: .cargo
...
$ grep ^a1b2c3d /mnt/linux/commits          # resolve an oid prefix
a1b2c3d4e5f6...  (full 40-char oid)
$ sudo umount /mnt/linux
```

## Why gitmount?

Comparing files across revisions normally means juggling `git worktree
add` / `git archive`, or cloning twice. gitmount makes **every** ref and
commit of a local repository appear as a plain directory tree, once, and
lets the tools you already have do the rest:

- **Diff anything against anything.** `diff -r /mnt/tag/v6.12
  /mnt/tag/v6.13` — branches, tags, remotes and raw commits are all just
  directories.
- **Safe by construction.** The mount is forcibly
  `ro,nosuid,nodev,default_permissions`; every write path returns
  `EROFS`. Nothing you do through the mount can modify the repository.
- **Ordinary tooling.** Editors, `grep`, `find`, `tar`, `rsync`, IDEs —
  no git knowledge required on the consumer side.
- **Self-contained.** A single `mount(8)` helper built on
  [libgit2](https://libgit2.com) and [libfuse3](https://github.com/libfuse/libfuse);
  no `git` subprocesses, no network access.

> **A note on naming**: during early development this project was briefly
> called "gitfs". That name belongs to
> [presslabs/gitfs](https://github.com/presslabs/gitfs) — a different,
> older project (Python, read-write, cloud-storage backends) — hence the
> rename to **gitmount**. See the [RFC](rfc/0000-gitmount.md) for the
> comparison.

## Features

- Full tree semantics: directories, regular/executable files, symlinks
  (raw-byte names included), submodules as empty dirs + marker files
- All ref namespaces: `branch/`, `tag/` (annotated tags peeled),
  `remote/`, `HEAD/`, and any commit by full oid under `commit/`
- Live refs: branches and tags created after mounting show up without a
  remount; already-resolved objects stay accessible
- Deterministic listings: every directory enumerates in raw byte
  (memcmp) dictionary order — stable, reproducible, locale-free
- Caching that keeps its promises: blob LRU + libgit2 tree cache;
  oversized blobs decompress **exactly once per open** (asserted by
  tests, observable via `-v`)
- Hardened baseline: `refs/replace` never followed, non-UTF-8 names
  passed through verbatim, pathological hand-crafted objects skipped
  with warnings
- First-class `mount(8)` citizen: `mount -t gitmount`, `/etc/fstab`
  entries and direct invocation are all equivalent; man page included

## Requirements

| Dependency | Version | Debian/Ubuntu | Fedora | Arch |
|---|---|---|---|---|
| C++ compiler | C++17 | `build-essential` | `gcc-c++` | `gcc` |
| CMake | ≥ 3.16 | `cmake` | `cmake` | `cmake` |
| pkg-config | — | `pkg-config` | `pkgconf` | `pkgconf` |
| [libgit2](https://libgit2.com) | ≥ 1.4 | `libgit2-dev` | `libgit2-devel` | `libgit2` |
| [libfuse3](https://github.com/libfuse/libfuse) | ≥ 3.10 | `libfuse3-dev` | `fuse3-devel` | `fuse3` |

Unit tests fetch [Catch2 v3](https://github.com/catchorg/Catch2) via
CMake FetchContent (network at configure time), or point
`FETCHCONTENT_SOURCE_DIR_CATCH2` at a local copy. Integration tests
need `/dev/fuse` + `fusermount3` and skip cleanly otherwise.

## Installation

### Arch Linux (AUR)

```sh
paru -S gitmount          # or: yay -S gitmount
```

No AUR helper needed:

```sh
git clone https://aur.archlinux.org/gitmount.git
cd gitmount
makepkg -si
```

The package builds from the release source tarball and runs the full
test suite in `check()` — the integration tests perform real FUSE
mounts and self-skip where `/dev/fuse` is unavailable. It installs
`/usr/bin/mount.gitmount` together with its man page
(`man 8 mount.gitmount`).

### Binary tarballs

Each [GitHub release](https://github.com/OrbitZore/gitmount/releases)
attaches prebuilt tarballs for two glibc bases (2.35 / 2.39), with
SPDX SBOMs, SHA-256 checksums and build attestations.

### From source

```sh
git clone https://github.com/OrbitZore/gitmount
cd gitmount
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build             # optional: unit + integration
sudo cmake --install build         # /usr/sbin/mount.gitmount + man8 page
```

## Quick start

The three equivalent invocation forms:

```sh
sudo mount -t gitmount /path/to/repo.git /mnt/gitmount     # via mount(8)
sudo mount /mnt/gitmount                                # via /etc/fstab
sudo mount.gitmount /path/to/repo.git /mnt/gitmount        # direct
```

`/etc/fstab` example (auto-mount at boot, survive missing repo):

```
/srv/repos/linux.git  /mnt/linux  gitmount  ro,noatime,nofail  0  0
```

Unmount with `sudo umount /mnt/gitmount` (or `fusermount3 -u`). The daemon
also exits gracefully on `SIGINT`/`SIGTERM`. Validate a configuration
without mounting — including repository readability — with `-f`:

```sh
mount.gitmount /srv/repos/linux.git /mnt/linux -f && echo config OK
```

## Usage

### Layout

```
/mnt/gitmount/
├── branch/      # local branches (nested names render as directories)
├── tag/         # tags, peeled to commits
├── commit/      # any commit by full oid — never listable
├── remote/      # remote-tracking refs: <remote>/<branch>
├── HEAD/        # snapshot of the current HEAD
├── commits      # every reachable commit oid, one per line
└── .gitmount.json  # mount metadata (immutable snapshot)
```

### Everyday tasks

```sh
diff -r /mnt/tag/v6.12 /mnt/tag/v6.13              # compare two tags
grep -rn "TODO" /mnt/branch/topic-branch/src       # search a branch
grep ^a1b2c3d /mnt/commits                         # resolve an oid prefix
rsync -a /mnt/commit/<oid>/ /tmp/snapshot/         # materialize a commit
tar -C /mnt/tag/v6.13 -czf v6.13.tgz .             # archive a tag
```

`/commit/<oid>` accepts only the full lowercase hex oid (40 or 64
chars); `commit/` itself is never listable by design — `grep` the
`commits` file instead.

### Options

```
-o blob-cache-size=<MiB>    blob LRU cache limit (default 64)
-o tree-cache-size=<MiB>    libgit2 tree/commit cache budget (default 256)
--blob-cache-size <MiB>     same as -o blob-cache-size=<MiB> (joined = form works too)
--tree-cache-size <MiB>     same as -o tree-cache-size=<MiB>
--foreground                stay in the foreground (default: daemonize)
-f                          fake: validate arguments and repository, don't mount
-v, --verbose               resolution logs + one line per blob decompression
--version, --help
```

`rw` in `-o` is accepted as a no-op with a warning (`mount(8)` pre-seeds
it unconditionally). Irrelevant VFS keys (`noatime`, `nofail`, `user`,
…) are accepted and ignored. `suid`, `dev`, `remount`, `uid=`, `gid=`,
`umask=`, `context=`-family options and `subtype=` are rejected — the
read-only baseline is not negotiable. Anything else passes through to
libfuse (e.g. `kernel_cache`, `allow_other`).

Exit codes: `0` success · `1` parameter error · `2` repository
unreadable · `3` mount failure.

## Documentation

- [docs/filesystem-semantics.md](docs/filesystem-semantics.md) — the
  stable, user-facing semantics contract
- [docs/mount.gitmount.8](docs/mount.gitmount.8) — the manual page (also
  installed as `man 8 mount.gitmount`)
- [rfc/0000-gitmount.md](rfc/0000-gitmount.md) — the normative design document
  (Chinese)
- [docs/maintenance-checklist.md](docs/maintenance-checklist.md) —
  operational checklists and measured performance baselines

## Performance

Measured on a 5,000-file synthetic tree (details in the
[maintenance checklist](docs/maintenance-checklist.md)):

| Workload | ext4 | gitmount |
|---|---|---|
| `find -type f` (metadata walk) | 6 ms | 49 ms |
| open+read+close, 4-deep path | 5 µs | ~350 µs |
| direct blob reads (libgit2, no FUSE) | — | 4.1 µs/blob |

Content-heavy loads scale with the data plane; metadata storms over
tiny files are latency-bound because metadata timeouts are pinned to
zero (a moved or deleted ref must be visible immediately — correctness
first). Oversized blobs decompress exactly once per open.

## Semantics worth knowing

The sharp edges pinned by the design (full contract:
[docs/filesystem-semantics.md](docs/filesystem-semantics.md)):

- **`/commits` pauses on first open** — a full revision walk runs once,
  on the first `open()`. First `cat` on a Linux-kernel-scale repository
  blocks for seconds; `stat` before that reports size 0 and never
  triggers it.
- **Oversized blobs pin at open** — a blob at/above the blob-cache limit
  decompresses once at `open()` and stays in memory until close.
  Opening a multi-GB blob takes seconds; concurrent requests are not
  blocked (decompression runs outside the global lock).
- **Disable gc while mounted (recommended)** — external
  `git gc`/`git prune` can cause transient `ENOENT`/`EIO` for in-flight
  requests; they recover once gc finishes. Or set
  `git config gc.auto 0` on long-mounted repositories.
- **`refs/replace` is never followed** — output matches
  `git --no-replace-objects`.
- **st_ino is path-derived and never recycled** — budget tens of MB per
  million distinct touched paths.

## FAQ

**Can I write through the mount?** No — every write path returns
`EROFS`. gitmount is a read-only data plane by design.

**Is it safe to mount untrusted repositories?** Symlink targets are
repository-controlled and passed through verbatim (like `git
checkout`), and paths are confined to the mountpoint by the VFS. Do not
browse untrusted repositories as root.

**sha256 repositories?** Yes — oids are 64 hex chars throughout.

**Why does `df` show 100% usage?** A read-only volume has no writable
space; `statfs` reports the local object database as the volume size
and zero free space. Alternates-backed storage is not counted.

**Why do some filenames look garbled?** Tree entry names are raw bytes;
a non-UTF-8 name appears exactly as it would after `git checkout` — no
escaping, no renaming.

**How do I match mounts?** `/proc/mounts` shows the type as
`fuse.gitmount` with source `gitmount`: `findmnt -t fuse.gitmount`.

**Wasn't this called gitfs?** During early development, yes. That name
belongs to [presslabs/gitfs](https://github.com/presslabs/gitfs) (Python,
read-write, cloud backends — a different project), so this project became
**gitmount**.

## Contributing

Contributions follow [CONTRIBUTING.md](CONTRIBUTING.md) (Conventional
Commits, tests required, `-Werror` CI). The community standards are in
[CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md); security issues follow
[SECURITY.md](SECURITY.md); changes are tracked in
[CHANGELOG.md](CHANGELOG.md).

## Acknowledgments

Built on [libgit2](https://libgit2.com),
[libfuse](https://github.com/libfuse/libfuse) and
[Catch2](https://github.com/catchorg/Catch2); validated against the
behavior of [util-linux](https://github.com/util-linux/util-linux)
mount(8) and git itself. The design was shaped by 39 rounds of review
recorded in the [RFC](rfc/0000-gitmount.md).

## License

[GPL-3.0-or-later](LICENSE) © The gitmount authors.
