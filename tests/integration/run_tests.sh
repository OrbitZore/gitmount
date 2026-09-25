#!/usr/bin/env bash
# gitmount — integration tests (RFC 0000 §5).
# SPDX-FileCopyrightText: 2026 The gitmount authors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs real FUSE mounts against the fixture repository and asserts the
# semantics pinned by RFC 0000 §3. Skipped (exit 0, marked) when /dev/fuse
# or fusermount3 is unavailable.
#
# Usage: run_tests.sh [path-to-mount.gitmount]
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
BIN=${1:-$ROOT/build/mount.gitmount}
export LC_ALL=C

PASS=0
FAIL=0
SKIPPED=0
declare -a MOUNT_PIDS=()

say()  { printf '%s\n' "$*"; }
pass() { PASS=$((PASS + 1)); say "ok      - $1"; }
fail() { FAIL=$((FAIL + 1)); say "FAIL    - $1"; }
skip() { SKIPPED=$((SKIPPED + 1)); say "skip    - $1"; }

check() {  # check <name> <command...>
  local name=$1; shift
  if "$@" >/dev/null 2>&1; then pass "$name"; else fail "$name"; fi
}
check_eq() {  # check_eq <name> <expected> <actual>
  local name=$1 want=$2 got=$3
  if [ "$want" = "$got" ]; then pass "$name"; else
    fail "$name (want=[$want] got=[$got])"
  fi
}

cleanup() {
  for m in "${MOUNT_PIDS[@]:-}"; do kill "$m" 2>/dev/null || true; done
  for d in "$TMP"/mnt-*; do [ -d "$d" ] && fusermount3 -u "$d" 2>/dev/null; done
}
trap cleanup EXIT

if [ ! -x "$BIN" ]; then echo "mount.gitmount not built: $BIN" >&2; exit 1; fi
if [ ! -e /dev/fuse ] || ! command -v fusermount3 >/dev/null; then
  skip "/dev/fuse unavailable — integration tests skipped"
  exit 0
fi

TMP=$(mktemp -d /tmp/gitmount-it.XXXXXX)
REPO="$TMP/repo"
"$ROOT/tests/fixtures/make_repo.sh" "$REPO" >/dev/null 2>&1
G() { git -C "$REPO" --no-replace-objects "$@"; }

# ---------------------------------------------------------------------------
# helpers: mount / umount
# ---------------------------------------------------------------------------
MNT=""
LOG=""
MOUNT_REPO=""   # override to mount a different repo (empty-repo section)
mount_gitmount() {  # mount_gitmount <name> [extra options...]
  local name=$1; shift
  local repo=${MOUNT_REPO:-$REPO}
  MNT="$TMP/mnt-$name"
  LOG="$TMP/log-$name"
  mkdir -p "$MNT"
  "$BIN" "$repo" "$MNT" --foreground -v "$@" >"$LOG" 2>&1 &
  local pid=$!
  MOUNT_PIDS+=("$pid")
  for _ in $(seq 1 100); do
    mountpoint -q "$MNT" 2>/dev/null && break
    kill -0 "$pid" 2>/dev/null || { say "mount failed: $(cat "$LOG")"; return 1; }
    sleep 0.05
  done
  mountpoint -q "$MNT"
}
umount_gitmount() {
  [ -n "$MNT" ] || return 0
  fusermount3 -u "$MNT" 2>/dev/null
  for _ in $(seq 1 100); do mountpoint -q "$MNT" 2>/dev/null || break; sleep 0.05; done
  MNT=""
}

# ---------------------------------------------------------------------------
# static CLI checks (no mount needed)
# ---------------------------------------------------------------------------
check "cli: missing args exit 1" sh -c "'$BIN' >/dev/null 2>&1; test \$? -eq 1"
check "cli: nonexistent repo exit 2" sh -c "'$BIN' '$TMP/nope' '$TMP/x' >/dev/null 2>&1; test \$? -eq 2"
check "cli: fake ok exit 0" sh -c "'$BIN' '$REPO' '$TMP/x' -f >/dev/null 2>&1"
check "cli: fake bad repo exit 2" sh -c "'$BIN' '$TMP' '$TMP/x' -f >/dev/null 2>&1; test \$? -eq 2"
check "cli: -o uid=1000 exit 1" sh -c "'$BIN' '$REPO' '$TMP/x' -o uid=1000 >/dev/null 2>&1; test \$? -eq 1"
check "cli: -o remount exit 1" sh -c "'$BIN' '$REPO' '$TMP/x' -o remount >/dev/null 2>&1; test \$? -eq 1"
check "cli: -o suid exit 1" sh -c "'$BIN' '$REPO' '$TMP/x' -o suid >/dev/null 2>&1; test \$? -eq 1"
check "cli: -o context= exit 1" sh -c "'$BIN' '$REPO' '$TMP/x' -o context=u:r:t:s0 >/dev/null 2>&1; test \$? -eq 1"
check "cli: -o subtype= exit 1" sh -c "'$BIN' '$REPO' '$TMP/x' -o subtype=x >/dev/null 2>&1; test \$? -eq 1"
check "cli: -o rw accepted" sh -c "'$BIN' '$REPO' '$TMP/x' -f -o rw >/dev/null 2>&1"
check "cli: -t gitmount accepted" sh -c "'$BIN' '$REPO' '$TMP/x' -t gitmount -f >/dev/null 2>&1"
check "cli: -t other rejected" sh -c "'$BIN' '$REPO' '$TMP/x' -t other >/dev/null 2>&1; test \$? -eq 1"
check "cli: options after positionals" sh -c "'$BIN' '$REPO' '$TMP/x' -f -o rw >/dev/null 2>&1"

# ---------------------------------------------------------------------------
# main mount
# ---------------------------------------------------------------------------
if mount_gitmount main; then
  C1=$(git -C "$REPO" rev-parse 'v1.0^{commit}')
  C2=$(git -C "$REPO" rev-parse v1.1)
  MAIN_TIP=$(git -C "$REPO" rev-parse main)
  DETACHED=$(git -C "$REPO" rev-parse HEAD)
  R1=$(git -C "$REPO" rev-parse refs/heads/replace-old)

  # --- root layout & ordering (byte-dict, §3.1/§3.3) ---
  check_eq "root listing order" \
    ".gitmount.json
HEAD
branch
commit
commits
remote
tag" "$(ls -A1 "$MNT" | LC_ALL=C sort)"

  # --- branch namespace ---
  check_eq "branch listing" \
    "blob-branch
dotdot
feature
main
pathological
replace-old" "$(ls -1 "$MNT/branch")"
  check "feature group is a directory" test -d "$MNT/branch/feature"
  check_eq "feature group listing" "x" "$(ls -1 "$MNT/branch/feature")"

  # --- content vs oracle ---
  check_eq "file content matches oracle" "hello world" \
    "$(cat "$MNT/branch/main/src/hello.txt")"
  check "executable bit" test -x "$MNT/branch/main/src/run.sh"
  check "chinese filename" test -f "$MNT/branch/main/docs/中文文档.txt"
  check "non-utf8 filename (raw bytes)" \
    bash -c "test -f \"$MNT/branch/main/\$(printf '\\xff\\xfe.bin')\""
  check_eq "symlink target" "README.md" "$(readlink "$MNT/branch/main/link-to-readme")"
  check "symlink traverses" test -f "$MNT/branch/main/link-to-readme"

  # --- metadata (§3.2) ---
  check_eq "group dir mode 0755" "755" "$(stat -c %a "$MNT/branch/feature")"
  MODE=$(stat -c %A "$MNT/branch/feature"); check "group dir is drwxr-xr-x" test "$MODE" = "drwxr-xr-x"
  check_eq "dir nlink=2" "2" "$(stat -c %h "$MNT/branch/feature")"
  check_eq "dir size 4096" "4096" "$(stat -c %s "$MNT/branch/feature")"
  MOUNT_AT=$(stat -c %Y "$MNT/branch")
  check_eq "group dir mtime = mount time" "$MOUNT_AT" "$(stat -c %Y "$MNT/branch/feature")"
  check_eq "tree dir mtime = committer time" "$(stat -c %Y "$MNT/branch/main")" \
    "$(stat -c %Y "$MNT/branch/main/src")"
  check_eq "blocks = ceil(size/512)" \
    "$(python3 -c "import os,math;print(math.ceil(os.stat('$MNT/branch/main/README.md').st_size/512))")" \
    "$(python3 -c "import os;print(os.stat('$MNT/branch/main/README.md').st_blocks)")"

  # --- tag namespace ---
  check_eq "tag listing" \
    "baz
foo
lw-blob
v1.0
v1.1" "$(ls -1 "$MNT/tag")"
  check_eq "annotated tag peels to commit" "hello gitmount" "$(cat "$MNT/tag/v1.0/README.md")"
  check "lightweight tag v1.1" test -f "$MNT/tag/v1.1/big/big.bin"
  check "tag to blob is ENOENT" test ! -e "$MNT/tag/lw-blob"
  check "branch to blob is ENOENT" test ! -e "$MNT/branch/blob-branch"

  # --- merged nodes: foo (D/F broken via packed-refs, §3.1) ---
  check_eq "merged node union order (aa ab bar zz)" \
    "aa
ab
bar
zz" "$(ls -1 "$MNT/tag/foo")"
  check "folded bar renders as directory" test -d "$MNT/tag/foo/bar"
  check_eq "longest-ref match: /tag/foo/bar is the sub-ref" \
    "from foo/bar ref" "$(cat "$MNT/tag/foo/bar/from-bar-ref" 2>/dev/null)"
  check "blob entry 'bar' shadowed by sub-ref (content differs)" \
    sh -c "! grep -q bar-original '$MNT/tag/foo/bar/from-bar-ref' 2>/dev/null"
  check_eq "tree entry aa content" "aa" "$(cat "$MNT/tag/foo/aa")"
  check_eq "tree entry zz content" "zz" "$(cat "$MNT/tag/foo/zz")"
  check "non-commit merged node: /tag/baz ENOENT (shape mismatch)" test ! -e "$MNT/tag/baz"
  # Shape mismatch per lookup semantics (§3.1): the parent listing keeps
  # the folded directory entry (an enumeration hint, not a promise), while
  # the node itself is ENOENT — deep paths through it are unreachable.
  check "parent listing keeps folded baz entry" ls "$MNT/tag" | grep -qx baz
  check "shape-mismatch warning logged" grep -q "refs/tags/baz does not peel" "$LOG"
  check "folding ambiguity warning logged" grep -q "folded ambiguity" "$LOG"

  # --- /commit (§3.1) ---
  check_eq "/commit readdir permanently empty" "" "$(ls -1 "$MNT/commit")"
  check_eq "/commit by full oid" "hello world" "$(cat "$MNT/commit/$MAIN_TIP/src/hello.txt")"
  TAG_OID=$(git -C "$REPO" rev-parse v1.0)  # annotated tag object oid
  check "/commit rejects non-commit oid (no peel)" test ! -e "$MNT/commit/$TAG_OID"
  check "/commit rejects uppercase oid" \
    test ! -e "$MNT/commit/$(echo "$MAIN_TIP" | tr a-z A-Z)"
  check "/commit rejects short prefix" test ! -e "$MNT/commit/${MAIN_TIP:0:8}"

  # --- refs/replace not honored (§3.1) ---
  check_eq "replace refs ignored (original R1 content)" "original R1" \
    "$(cat "$MNT/commit/$R1/which.txt")"
  R2_CONTENT=$(git -C "$REPO" cat-file blob "$(git -C "$REPO" rev-parse "refs/replace/$R1^{tree}:which.txt")")
  check "git default shows the replacement (oracle guard)" \
    sh -c "[ '$R2_CONTENT' = 'REPLACEMENT R2' ]"

  # --- /remote (§3.1) ---
  check_eq "remote namespaces" "legacy
origin
upstream" "$(ls -1 "$MNT/remote")"
  check_eq "origin listing (hidden symbolic HEAD)" "feature
main" "$(ls -1 "$MNT/remote/origin")"
  check "symbolic origin/HEAD hidden (ENOENT)" test ! -e "$MNT/remote/origin/HEAD"
  check "direct upstream/HEAD visible" test -d "$MNT/remote/upstream/HEAD"
  check_eq "upstream/HEAD content" "hello gitmount" "$(cat "$MNT/remote/upstream/HEAD/README.md")"
  check_eq "bare namespace legacy renders empty" "" "$(ls -1 "$MNT/remote/legacy")"
  check_eq "remote tracking branch content" "hello world" \
    "$(cat "$MNT/remote/origin/main/src/hello.txt")"

  # --- /HEAD (§3.1) ---
  check_eq "detached HEAD snapshot" "hello world" "$(cat "$MNT/HEAD/src/hello.txt")"
  check "HEAD has detached-note (unique commit)" test -f "$MNT/HEAD/detached-note.txt"

  # --- submodule markers (§3.2) ---
  check "submodule dir is empty" sh -c "[ -z \"\$(ls -A1 '$MNT/branch/main/deps/libfoo')\" ]"
  MARKER="$MNT/branch/main/deps/libfoo.gitmount-submodule"
  check "marker file exists" test -f "$MARKER"
  SUB_URL=$(git -C "$REPO" config -f .gitmodules --get submodule.deps/libfoo.url)
  SUB_OID=$(git -C "$REPO" rev-parse main:deps/libfoo)
  check_eq "marker content url" "url=$SUB_URL" "$(head -n1 "$MARKER")"
  check_eq "marker content commit" "commit=$SUB_OID" "$(tail -n1 "$MARKER")"
  check_eq "marker mode 0644" "644" "$(stat -c %a "$MARKER")"
  MARKER_MTIME=$(stat -c %Y "$MARKER")
  MAIN_MTIME=$(stat -c %Y "$MNT/branch/main")
  check "marker mtime = committer time" test "$MARKER_MTIME" = "$MAIN_MTIME"

  # --- symlinks: pathological forms (§3.2/§3.3) ---
  check_eq "embedded NUL truncated" "README.md" "$(readlink "$MNT/branch/pathological/nul-link")"
  check_eq "NUL-symlink st_size = truncated length" "9" \
    "$(stat -c %s "$MNT/branch/pathological/nul-link")"
  check "empty symlink target" sh -c "[ -z \"\$(readlink '$MNT/branch/pathological/empty-link')\" ]"
  check_eq "empty symlink st_size 0" "0" "$(stat -c %s "$MNT/branch/pathological/empty-link")"
  check ">=PATH_MAX no NUL: readlink ENAMETOOLONG" \
    sh -c "! readlink '$MNT/branch/pathological/long-link' 2>/dev/null"
  check_eq "no-NUL pathological st_size = full length" "5000" \
    "$(stat -c %s "$MNT/branch/pathological/long-link")"
  check "NUL after PATH_MAX: readlink ENAMETOOLONG" \
    sh -c "! readlink '$MNT/branch/pathological/tail-nul-link' 2>/dev/null"
  check_eq "tail-NUL pathological st_size = truncated 4500" "4500" \
    "$(stat -c %s "$MNT/branch/pathological/tail-nul-link")"

  # --- reserved-name entries skipped (§3.1) ---
  check_eq "dotdot tree listing skips . and .." "real" "$(ls -1A "$MNT/branch/dotdot")"
  check_eq "find sees only the real entry" "real" "$(find "$MNT/branch/dotdot" -mindepth 1 -maxdepth 1 -printf '%f\n')"
  check "reserved-name warning logged" grep -q "reserved name" "$LOG"

  # --- commits list (§3.5) ---
  check_eq "commits st_size 0 before first open" "0" "$(stat -c %s "$MNT/commits")"
  GITMOUNT_LIST=$(cat "$MNT/commits")
  check "commits non-empty after open" test -n "$GITMOUNT_LIST"
  ORACLE=$(git -C "$REPO" --no-replace-objects rev-list --all HEAD | LC_ALL=C sort)
  check_eq "commits set == rev-list --all HEAD (sorted)" "$ORACLE" \
    "$(printf '%s\n' "$GITMOUNT_LIST" | LC_ALL=C sort)"
  check "detached-only commit in list" sh -c "printf '%s\n' '$GITMOUNT_LIST' | grep -qx '$DETACHED'"
  check "commits st_size > 0 after open" sh -c "[ \"\$(stat -c %s '$MNT/commits')\" -gt 0 ]"
  REOPEN=$(cat "$MNT/commits")
  check "commits reopen sees same version" test "$REOPEN" = "$GITMOUNT_LIST"

  # --- live ref updates visible without remount (§3.1) ---
  git -C "$REPO" tag livetag "$C1"
  check "new tag visible without remount" test -d "$MNT/tag/livetag"
  git -C "$REPO" tag -d livetag >/dev/null
  check "deleted tag disappears" test ! -e "$MNT/tag/livetag"

  # --- .gitmount.json (§3.6) ---
  JSON=$(cat "$MNT/.gitmount.json")
  check "json parses" sh -c "printf '%s' '$JSON' | python3 -m json.tool >/dev/null"
  check_eq "json head (detached)" "\"$DETACHED\"" \
    "$(printf '%s' "$JSON" | python3 -c 'import json,sys; print(json.dumps(json.load(sys.stdin)["head"]))')"
  check_eq "json repository" "\"$REPO\"" \
    "$(printf '%s' "$JSON" | python3 -c 'import json,sys; print(json.dumps(json.load(sys.stdin)["repository"]))')"
  check_eq "json cache blob_bytes" "67108864" \
    "$(printf '%s' "$JSON" | python3 -c 'import json,sys; print(json.load(sys.stdin)["cache"]["blob_bytes"])')"

  # --- statfs (§3.3) ---
  check_eq "statfs f_bfree = 0" "0" \
    "$(python3 -c "import os; print(os.statvfs('$MNT').f_bfree)")"
  check_eq "statfs f_bavail = 0" "0" \
    "$(python3 -c "import os; print(os.statvfs('$MNT').f_bavail)")"
  check "statfs f_blocks > 0" \
    python3 -c "import os; assert os.statvfs('$MNT').f_blocks > 0"
  check_eq "statfs block size 4096" "4096" \
    "$(python3 -c "import os; print(os.statvfs('$MNT').f_bsize)")"

  # --- read-only enforcement (§3.3) ---
  check "create fails EROFS" sh -c "! touch '$MNT/branch/main/newfile' 2>/dev/null"
  check "mkdir fails EROFS" sh -c "! mkdir '$MNT/branch/main/newdir' 2>/dev/null"
  check "rm fails EROFS" sh -c "! rm '$MNT/branch/main/README.md' 2>/dev/null"
  check "xattr queries report ENOTSUP" \
    sh -c "! getfattr -d '$MNT/branch/main/README.md' 2>/dev/null"

  # --- zero-decompress assertion: ls -l over a plain-blob-only dir (§5) ---
  ZDCOUNT=$(grep -c "decompressed" "$LOG" || true)
  ls -l "$MNT/branch/main/big" >/dev/null 2>&1
  ZDAFTER=$(grep -c "decompressed" "$LOG" || true)
  check_eq "zero decompression on plain-blob ls -l" "$ZDCOUNT" "$ZDAFTER"

  umount_gitmount
else
  skip "main mount failed — remaining main-mount assertions skipped"
fi

# ---------------------------------------------------------------------------
# oversized blob mount: --blob-cache-size=1 (§3.5)
# ---------------------------------------------------------------------------
if mount_gitmount oversize -o blob-cache-size=1; then
  ORACLE_HASH=$(git -C "$REPO" cat-file blob main:big/big.bin | sha256sum | cut -d' ' -f1)
  GOT_HASH=$(sha256sum < "$MNT/branch/main/big/big.bin" | cut -d' ' -f1)
  check "oversized blob (2MiB) content matches oracle" test "$ORACLE_HASH" = "$GOT_HASH"
  ORACLE_EXACT_HASH=$(git -C "$REPO" cat-file blob main:big/exact.bin | sha256sum | cut -d' ' -f1)
  GOT_EXACT_HASH=$(sha256sum < "$MNT/branch/main/big/exact.bin" | cut -d' ' -f1)
  check "boundary blob (exactly 1MiB = capacity) bypasses cache" \
    test "$ORACLE_EXACT_HASH" = "$GOT_EXACT_HASH"
  PINS=$(grep -c "oversized open-pin" "$LOG" || true)
  check_eq "open-pin decompression count == 2 (two files)" "2" "$PINS"
  # Pinned-per-open semantics (§3.5): ONE open reading in many chunks
  # decompresses exactly once — dd uses a single open + many reads, so the
  # count grows by exactly 1 (not by the number of read calls).
  dd if="$MNT/branch/main/big/big.bin" of=/dev/null bs=65536 2>/dev/null
  check_eq "chunked single-open read adds exactly one decompression" "$((PINS + 1))" \
    "$(grep -c 'oversized open-pin' "$LOG" || true)"
  # Cacheable small blob loads once, then hits.
  cat "$MNT/branch/main/big/small.bin" >/dev/null
  cat "$MNT/branch/main/big/small.bin" >/dev/null
  check_eq "cacheable blob: one miss load then hits" "1" \
    "$(grep -c 'cache miss load' "$LOG" || true)"
  umount_gitmount
else
  skip "oversize mount failed"
fi

# ---------------------------------------------------------------------------
# empty repository (§3.1)
# ---------------------------------------------------------------------------
EMPTY="$TMP/empty"
git init -q -b main "$EMPTY"
MOUNT_REPO="$EMPTY"
if mount_gitmount empty; then
  check_eq "empty repo: branch listing empty" "" "$(ls -1 "$MNT/branch")"
  check_eq "empty repo: tag listing empty" "" "$(ls -1 "$MNT/tag")"
  check_eq "empty repo: remote listing empty" "" "$(ls -1 "$MNT/remote")"
  check "empty repo: HEAD ENOENT" test ! -e "$MNT/HEAD"
  check_eq "empty repo: commits empty" "" "$(cat "$MNT/commits")"
  check_eq "empty repo: json head null" "None" \
    "$(python3 -c 'import json; print(json.load(open("'$MNT'/.gitmount.json"))["head"])')"
  umount_gitmount
else
  skip "empty-repo mount failed"
fi

# ---------------------------------------------------------------------------
# mount(8) exec scenarios (privileged; skip otherwise) — §3.7
# ---------------------------------------------------------------------------
if [ "$(id -u)" = "0" ] && command -v mount >/dev/null; then
  # mount(8) execs helpers from sbin; install the freshly built binary
  # there for the duration of the test (removed on exit).
  if [ ! -e /usr/sbin/mount.gitmount ]; then
    cp "$BIN" /usr/sbin/mount.gitmount && HELPER_INSTALLED=1
  else
    cp "$BIN" /usr/sbin/mount.gitmount && HELPER_INSTALLED=0  # overwrite, keep
  fi
  trap 'fusermount3 -u "$TMP"/mnt-* 2>/dev/null; umount "$TMP"/mnt-m8 2>/dev/null; \
        [ "${HELPER_INSTALLED:-0}" = 1 ] && rm -f /usr/sbin/mount.gitmount; \
        cleanup' EXIT
  M8="$TMP/mnt-m8"; mkdir -p "$M8"
  M8ERR="$TMP/m8.err"
  if mount -t gitmount "$REPO" "$M8" 2>"$M8ERR"; then
    check "mount(8) exec: default invocation mounts (rw preseed accepted)" \
      mountpoint -q "$M8"
    check "rw preseed warning on stderr" grep -q "rw ignored" "$M8ERR"
    check "/proc/mounts type fuse.gitmount with ro,nosuid,nodev" \
      sh -c "grep '$M8 ' /proc/mounts | grep -q 'fuse.gitmount ro,nosuid,nodev'"
    umount "$M8" >/dev/null 2>&1 || fusermount3 -u "$M8"
    if mount --fake -t gitmount "$REPO" "$M8" >/dev/null 2>&1; then
      pass "mount(8) exec: --fake validates without mounting"
    else
      fail "mount(8) exec: --fake"
    fi
    mountpoint -q "$M8" && fail "--fake must not mount" || pass "--fake does not mount"
    if mount -t gitmount -o noatime "$REPO" "$M8" >/dev/null 2>&1 && mountpoint -q "$M8"; then
      pass "mount(8) exec: -o noatime accepted"
      umount "$M8" >/dev/null 2>&1
    else
      fail "mount(8) exec: -o noatime"
    fi
    if mount -t gitmount -o noatime,nodiratime "$REPO" "$M8" >/dev/null 2>&1 && mountpoint -q "$M8"; then
      pass "mount(8) exec: -o noatime,nodiratime combo"
      umount "$M8" >/dev/null 2>&1
    else
      fail "mount(8) exec: -o noatime,nodiratime combo"
    fi
    if mount -t gitmount -o user=alice "$REPO" "$M8" >/dev/null 2>&1 && mountpoint -q "$M8"; then
      pass "mount(8) exec: -o user=<name> key-value form"
      umount "$M8" >/dev/null 2>&1
    else
      fail "mount(8) exec: -o user=<name>"
    fi
    if mount -t gitmount -o nouser "$REPO" "$M8" >/dev/null 2>&1 && mountpoint -q "$M8"; then
      pass "mount(8) exec: -o nouser (table-track no-op)"
      umount "$M8" >/dev/null 2>&1
    else
      fail "mount(8) exec: -o nouser"
    fi
  else
    skip "mount -t gitmount unavailable in this environment"
  fi
else
  skip "mount(8) exec scenarios need root"
fi

# ---------------------------------------------------------------------------
say ""
say "passed: $PASS  failed: $FAIL  skipped: $SKIPPED"
[ "$FAIL" -eq 0 ]
