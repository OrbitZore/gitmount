# Contributing to gitfs

Thanks for your interest in improving gitfs! This project follows the
engineering conventions pinned in its design document
([rfc/0000-gitfs.md](rfc/0000-gitfs.md) §5).

## Getting started

```sh
git clone https://github.com/OrbitZore/gitfs
cd gitfs
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DGITFS_WERROR=ON
cmake --build build
ctest --test-dir build          # unit tests always; integration needs /dev/fuse
```

Integration tests perform real FUSE mounts; they skip cleanly when
`/dev/fuse` or `fusermount3` is unavailable. To also exercise the
`mount(8)` exec-helper scenarios, run the integration script as root
(`sudo tests/integration/run_tests.sh build/mount.gitfs`).

## Development workflow

1. **Discuss first for semantic changes.** gitfs's behavior is pinned by
   RFC 0000; anything that changes observable filesystem semantics needs a
   design discussion (issue or RFC amendment) before implementation.
   Bug fixes that realign behavior with the RFC do not.
2. **Branch and commit** using [Conventional Commits](https://www.conventionalcommits.org)
   (`feat:`, `fix:`, `docs:`, `test:`, `refactor:`, `chore:`, ...). CI
   enforces the format on the commit range of every PR.
3. **Keep the suites green.** New behavior comes with unit tests (pure
   logic in `src/` is deliberately structured for testability) and, when
   it affects mount-visible semantics, integration assertions in
   `tests/integration/run_tests.sh` (add the fixture shape to
   `tests/fixtures/make_repo.sh` if needed).
4. **Format** with the repository's `.clang-format` (Google style):
   `clang-format -i src/** tests/**`. CI checks formatting.
5. **Warnings are errors in CI** (`-Wall -Wextra -Wpedantic -Werror`).

## Semantics-relevant checklists

- New or changed `-o` keys: update the triage tracks in `src/options.cpp`,
  the unit tests, `docs/mount.gitfs.8`, and possibly
  `docs/maintenance-checklist.md`.
- New user-visible behavior: update `docs/filesystem-semantics.md` and the
  man page (bump its decision watermark).
- Performance-relevant changes: keep the decompression-counter assertions
  green; run the §3.5 performance gates on a quiet machine.

## Reporting bugs

Open an issue with: the gitfs version (`mount.gitfs --version`), the
libgit2/libfuse versions, the reproduction steps (ideally against a
repository shape the fixture script can express), and verbose logs
(`-v`) where relevant. Do not browse untrusted repositories as root
(see [SECURITY.md](SECURITY.md)).

## Licensing

By contributing you agree that your contributions are licensed under
GPL-3.0-or-later, like the rest of the project.
