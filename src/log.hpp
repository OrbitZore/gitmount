// gitmount — read-only git-to-FUSE filesystem (RFC 0000).
// SPDX-FileCopyrightText: 2026 The gitmount authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdarg>

namespace gitmount::log {

// Verbosity gate: set once from -v/--verbose before the FUSE loop starts.
// Reads are unsynchronized; the flag is written exactly once at startup,
// before any worker thread exists (safe under the FUSE threading model).
void set_verbose(bool enabled);
bool verbose();

// All sinks write to stderr with a "gitmount: <level>: " prefix. Each call is a
// single fprintf (POSIX per-stream locking makes individual calls atomic).
[[gnu::format(printf, 1, 2)]] void error(const char* fmt, ...);
[[gnu::format(printf, 1, 2)]] void warn(const char* fmt, ...);
[[gnu::format(printf, 1, 2)]] void info(const char* fmt, ...);
// vlog: only emitted when verbose mode is on (RFC 0000 §3.7). Used for the
// decompression counter (§3.5) and ignored-option notices (§3.7).
[[gnu::format(printf, 1, 2)]] void vlog(const char* fmt, ...);

}  // namespace gitmount::log
