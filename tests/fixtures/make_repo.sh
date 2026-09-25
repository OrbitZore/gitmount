#!/usr/bin/env bash
# gitmount — fixture repository generator (RFC 0000 §5).
# SPDX-FileCopyrightText: 2026 The gitmount authors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the pinned fixture repo covering every integration assertion:
#   - submodule, executable, Chinese filename, non-UTF-8 filename
#   - nested branch (feature/x), annotated tag, lightweight tag -> blob
#   - symlinks: embedded NUL, >= PATH_MAX without NUL, first NUL after
#     PATH_MAX (5000 bytes, NUL at 4500), empty target
#   - big-blob directory with ONLY plain blobs (zero-decompress assertion):
#     2 MiB + exactly 1 MiB (bypass boundary >=) + 100 KiB
#   - hand-crafted tree with "." and ".." entries (fsck bypass)
#   - detached HEAD with a unique commit; origin/HEAD symbolic (hidden);
#     upstream/HEAD direct (ordinary ref); bare refs/remotes/legacy
#   - hand-edited packed-refs breaking D/F: tags foo + foo/ab + foo/bar
#     (foo's tree holds blobs aa/bar/zz), baz -> blob + baz/qux (shape
#     mismatch merged node)
#   - refs/replace/<oid>, refs/notes/keep, refs/stash, branch -> blob
#
# Usage: make_repo.sh <target-dir>
set -euo pipefail

DEST=${1:?usage: make_repo.sh <target-dir>}
if [ -e "$DEST" ]; then echo "refusing to overwrite $DEST" >&2; exit 1; fi

export GIT_AUTHOR_NAME=gitmount GIT_AUTHOR_EMAIL=gitmount@example.com
export GIT_COMMITTER_NAME=gitmount GIT_COMMITTER_EMAIL=gitmount@example.com
export GIT_AUTHOR_DATE="2005-04-07T22:13:13 +0000"
export GIT_COMMITTER_DATE="2005-04-07T22:13:13 +0000"

G="git -C $DEST"

# ---------------------------------------------------------------------------
# submodule source repository (sibling of the fixture)
# ---------------------------------------------------------------------------
SUB_SRC="$DEST-sub-src"
git init -q -b main "$SUB_SRC"
printf 'submodule content\n' > "$SUB_SRC/lib.txt"
git -C "$SUB_SRC" add lib.txt
git -C "$SUB_SRC" commit -qm "submodule root"

# ---------------------------------------------------------------------------
# main history
# ---------------------------------------------------------------------------
git init -q -b main "$DEST"
cd "$DEST"

mkdir -p src docs sub
printf 'hello gitmount\n' > README.md
printf 'hello world\n' > src/hello.txt
printf '#!/bin/sh\necho run\n' > src/run.sh
chmod 755 src/run.sh
printf '中文文档内容\n' > "docs/中文文档.txt"
printf 'binary name\n' > $'\xff\xfe.bin'
printf 'sub content\n' > sub/inner.txt
ln -s README.md link-to-readme
git add -A
git commit -qm "c1: base tree with modes, names, symlink"
C1=$(git rev-parse HEAD)

mkdir -p big
head -c 2097152 /dev/urandom > big/big.bin
head -c 1048576 /dev/urandom > big/exact.bin      # exactly 1 MiB: >= boundary
head -c 102400 /dev/urandom > big/small.bin       # cacheable
git add big
git commit -qm "c2: plain-blob directory for zero-decompress assertion"
C2=$(git rev-parse HEAD)

git -c protocol.file.allow=always submodule add -q "$SUB_SRC" deps/libfoo
git add -A
git commit -qm "c3: submodule at deps/libfoo"
C3=$(git rev-parse HEAD)

git branch feature/x "$C2"
git checkout -q feature/x
printf 'feature work\n' > feature.txt
git add feature.txt
git commit -qm "fx: work on feature/x"
git checkout -q main

# Detached HEAD with a unique commit (commits-list assertion). Built
# via a scratch index so the worktree stays clean.
printf 'detached only\n' > detached-note.txt
git add detached-note.txt
DTREE=$(git write-tree)
git reset -q
rm -f detached-note.txt
DETACHED=$(git commit-tree "$DTREE" -p "$C3" -m "detached-only")
git checkout -q --detach "$DETACHED"

# ---------------------------------------------------------------------------
# hand-crafted objects (mktree / hash-object; fsck bypasses are intentional)
# ---------------------------------------------------------------------------
mkblob() { git hash-object -w --stdin; }

EMPTY_LINK=$(mkblob </dev/null)                                   # empty target
NUL_LINK=$(printf 'README.md\0hidden' | mkblob)                  # embedded NUL
LONG_LINK=$(python3 -c 'import sys; sys.stdout.write("x"*5000)' | mkblob)
TAIL_NUL_LINK=$(python3 -c 'import sys; sys.stdout.write("y"*4500+"\0"+"z"*499)' | mkblob)
NORMAL=$(printf 'plain file\n' | mkblob)
AA=$(printf 'aa\n' | mkblob)
BAR_ORIG=$(printf 'bar-original (shadowed by sub-ref)\n' | mkblob)
ZZ=$(printf 'zz\n' | mkblob)
DOT_BLOB=$(printf 'dot\n' | mkblob)

PTREE=$(printf '100644 blob %s\tempty-link\n120000 blob %s\tlong-link\n120000 blob %s\tnormal.txt\n120000 blob %s\tnul-link\n120000 blob %s\ttail-nul-link\n' \
  "$EMPTY_LINK" "$LONG_LINK" "$NORMAL" "$NUL_LINK" "$TAIL_NUL_LINK" | git mktree)
PCOMMIT=$(git commit-tree "$PTREE" -m "pathological symlinks")
git update-ref refs/heads/pathological "$PCOMMIT"

DOTTREE=$(printf '100644 blob %s\t.\n100644 blob %s\t..\n100644 blob %s\treal\n' \
  "$DOT_BLOB" "$DOT_BLOB" "$DOT_BLOB" | git mktree)
DOTCOMMIT=$(git commit-tree "$DOTTREE" -m "reserved-name entries (fsck bypass)")
git update-ref refs/heads/dotdot "$DOTCOMMIT"

# Merged-node pieces: foo's tree holds aa, bar (blob!), zz — the sub-ref
# foo/bar shadows the blob when folded.
FOOTREE=$(printf '100644 blob %s\taa\n100644 blob %s\tbar\n100644 blob %s\tzz\n' \
  "$AA" "$BAR_ORIG" "$ZZ" | git mktree)
FOOCOMMIT=$(git commit-tree "$FOOTREE" -m "merged node base (tag foo)")
ABC=$(printf 'from foo/ab ref\n' | mkblob)
ABCOMMIT=$(git commit-tree "$(printf '100644 blob %s\tfrom-ab\n' "$ABC" | git mktree)" -m "tag foo/ab")
BARREF=$(printf 'from foo/bar ref\n' | mkblob)
BARCOMMIT=$(git commit-tree "$(printf '100644 blob %s\tfrom-bar-ref\n' "$BARREF" | git mktree)" -m "tag foo/bar")
QUX=$(printf 'from baz/qux ref\n' | mkblob)
QUXCOMMIT=$(git commit-tree "$(printf '100644 blob %s\tfrom-qux\n' "$QUX" | git mktree)" -m "tag baz/qux")

# ---------------------------------------------------------------------------
# tags / branches with non-commit targets
# ---------------------------------------------------------------------------
git tag -a v1.0 -m "annotated v1.0" "$C1"
git tag v1.1 "$C2"
BLOB_TAG=$(printf 'i am a blob\n' | mkblob)
git update-ref refs/tags/lw-blob "$BLOB_TAG"

# ---------------------------------------------------------------------------
# remote-tracking namespace fixtures
# ---------------------------------------------------------------------------
git update-ref refs/remotes/origin/main "$C3"
git update-ref refs/remotes/origin/feature/x "$(git rev-parse feature/x)"
git symbolic-ref refs/remotes/origin/HEAD refs/remotes/origin/main
git update-ref refs/remotes/upstream/HEAD "$C1"      # direct: ordinary ref
git update-ref refs/remotes/legacy "$C2"             # bare namespace ref

# ---------------------------------------------------------------------------
# special namespaces for the commits-list oracle parity
# ---------------------------------------------------------------------------
git update-ref refs/notes/keep "$C1"
git update-ref refs/stash "$C2"

# refs/replace: gitmount never follows replacements (RFC 0000 §3.1).
R1_TREE=$(printf '100644 blob %s\twhich.txt\n' "$(printf 'original R1\n' | mkblob)" | git mktree)
R1=$(git commit-tree "$R1_TREE" -p "$C1" -m "R1 original")
R2_TREE=$(printf '100644 blob %s\twhich.txt\n' "$(printf 'REPLACEMENT R2\n' | mkblob)" | git mktree)
R2=$(git commit-tree "$R2_TREE" -p "$C1" -m "R2 replacement")
git update-ref refs/heads/replace-old "$R1"
git update-ref "refs/replace/$R1" "$R2"

# ---------------------------------------------------------------------------
# pack refs, then hand-edit packed-refs to break D/F (RFC 0000 §5 fixture)
# ---------------------------------------------------------------------------
git pack-refs --all
PACKEDREFS=.git/packed-refs
{
  echo "$FOOCOMMIT refs/tags/foo"
  echo "$ABCOMMIT refs/tags/foo/ab"
  echo "$BARCOMMIT refs/tags/foo/bar"
  echo "$BLOB_TAG refs/tags/baz"
  echo "$QUXCOMMIT refs/tags/baz/qux"
} >> "$PACKEDREFS"
# The appended lines break the file's sort order; libgit2's packed-refs
# lookup binary-searches (assuming sorted names), so re-sort the whole
# body with ^peel lines kept attached to their entries.
python3 - "$PACKEDREFS" <<'PYEOF'
import sys
path = sys.argv[1]
lines = open(path).read().splitlines()
header = [l for l in lines if l.startswith('#')]
body = [l for l in lines if not l.startswith('#')]
entries = []
i = 0
while i < len(body):
    parts = body[i].split()
    oid, name = parts[0], parts[1]
    peel = None
    if i + 1 < len(body) and body[i + 1].startswith('^'):
        peel = body[i + 1].lstrip('^').split()[0]
        i += 1
    entries.append((name, oid, peel))
    i += 1
entries.sort(key=lambda e: e[0].encode())
with open(path, 'w') as f:
    for h in header:
        f.write(h + '\n')
    for name, oid, peel in entries:
        f.write('%s %s\n' % (oid, name))
        if peel:
            f.write('^%s\n' % peel)
PYEOF

# gc for a clean packfile-backed ODB; ref re-packing is disabled because
# the blob-branch loose ref (written below) violates update-ref's safety
# check, and gc/pack-refs would choke on it.
git -c gc.packRefs=false -c gc.reflogExpire=never \
  -c gc.reflogExpireUnreachable=never gc -q 2>/dev/null || true

# Branch pointing at a blob (non-commit ref entry: ENOENT assertion).
# Written after gc for the same safety-check reason.
mkdir -p .git/refs/heads
printf '%s\n' "$BLOB_TAG" > .git/refs/heads/blob-branch

# Sanity: the hand-packed merged nodes must be visible.
git rev-parse --verify -q refs/tags/foo >/dev/null
git rev-parse --verify -q refs/tags/foo/bar >/dev/null
git rev-parse --verify -q refs/tags/baz >/dev/null

echo "fixture ready: $DEST" >&2
