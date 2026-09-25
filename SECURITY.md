# Security Policy

## Supported versions

Security fixes are applied to the latest released version (see
[CHANGELOG.md](CHANGELOG.md) and the git tags; v0.1.x tracks the `0.x`
line until a stable 1.0).

## Threat model (summary)

gitmount is a read-only, content-addressed data plane: it performs no network
access and offers no write path. The relevant risks are (a) untrusted
repository contents — symlink targets, objects crafted to bypass `git
fsck`, overlong names — and (b) repository mutation by external processes
during a mount. See RFC 0000 §4 for the full analysis.

Practical guidance:

- Never browse untrusted repositories as root / with privileges; symlink
  targets are passed through verbatim (same semantics as `git checkout`).
- Mounts are forced `ro,nosuid,nodev,default_permissions`; `suid`, `dev`
  and SELinux labeling options are rejected at parse time.
- `refs/replace` is never followed, so a replaced object cannot be used to
  smuggle different content under a known oid.

## Reporting a vulnerability

Please report privately via
[GitHub security advisories](https://github.com/OrbitZore/gitmount/security/advisories/new)
("Report a vulnerability"), or contact the maintainers directly (see
`git shortlog -sne` for active maintainers). Include:

1. The gitmount version (`mount.gitmount --version`) and library versions.
2. A minimal reproduction — ideally a repository shape describable with
   `tests/fixtures/make_repo.sh` operations.
3. Impact assessment and any mitigations you have considered.

We will respond within 7 days, coordinate a fix and disclosure timeline
with you, and credit the report unless you prefer otherwise.

Please do not open public issues for exploitable defects.
