// gitmount — read-only git-to-FUSE filesystem (RFC 0000).
// SPDX-FileCopyrightText: 2026 The gitmount authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "log.hpp"

#include <cstdio>

namespace gitmount::log {

namespace {
bool g_verbose = false;

void emit(const char* level, const char* fmt, va_list ap) {
  std::fprintf(stderr, "gitmount: %s: ", level);
  std::vfprintf(stderr, fmt, ap);
  std::fputc('\n', stderr);
}
}  // namespace

void set_verbose(bool enabled) { g_verbose = enabled; }
bool verbose() { return g_verbose; }

void error(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  emit("error", fmt, ap);
  va_end(ap);
}

void warn(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  emit("warning", fmt, ap);
  va_end(ap);
}

void info(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  emit("info", fmt, ap);
  va_end(ap);
}

void vlog(const char* fmt, ...) {
  if (!g_verbose) return;
  va_list ap;
  va_start(ap, fmt);
  emit("verbose", fmt, ap);
  va_end(ap);
}

}  // namespace gitmount::log
