## Summary

<!-- What does this PR change and why? Reference the issue if one exists. -->

## Type of change

<!-- Check one (Conventional Commits mirror). -->

- [ ] `feat:` new behavior (semantics-affecting — see checklist below)
- [ ] `fix:` bug fix (aligns behavior with RFC 0000 / semantics doc)
- [ ] `docs:` / `test:` / `refactor:` / `chore:` non-semantic change

## Checklist

- [ ] Commit messages follow Conventional Commits (CI enforces).
- [ ] Unit tests added/updated for new logic.
- [ ] Integration assertions added when mount-visible behavior changes
      (update `tests/fixtures/make_repo.sh` if a new repository shape is
      needed).
- [ ] `clang-format -i` applied to touched sources (CI enforces).
- [ ] No new compiler warnings (`-Wall -Wextra -Wpedantic -Werror`).

### If observable filesystem semantics changed

- [ ] `docs/filesystem-semantics.md` updated.
- [ ] `docs/mount.gitfs.8` updated (bump the decision watermark in its
      header comment).
- [ ] `README.md` and `README.zh-CN.md` updated together (both languages
      stay in sync).

### If cache or lock behavior changed

- [ ] The decompression-counter assertions in the integration suite are
      still green ("exactly one decompression per open").
