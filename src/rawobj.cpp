// gitmount — read-only git-to-FUSE filesystem (RFC 0000).
// SPDX-FileCopyrightText: 2026 The gitmount authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "rawobj.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace gitmount::rawobj {

namespace {

const char* parse_mode_prefix(const char* p, const char* end, std::uint32_t* mode_out) {
  std::uint32_t mode = 0;
  bool any = false;
  while (p < end && *p >= '0' && *p <= '7') {
    mode = (mode << 3) | static_cast<std::uint32_t>(*p - '0');
    any = true;
    ++p;
  }
  if (!any || p >= end || *p != ' ') return nullptr;
  *mode_out = mode;
  return p + 1;  // past the space
}

bool oid_from_hex(const char* hex, std::size_t len, git_oid_t oid_type, git_oid* out) {
  char buf[GIT_OID_MAX_HEXSIZE + 1];
  const std::size_t want = oid_type == GIT_OID_SHA1 ? 40 : 64;
  if (len != want || len > sizeof(buf) - 1) return false;
  std::memcpy(buf, hex, len);
  buf[len] = '\0';
  return git_oid_fromstr(out, buf) == 0;
}

}  // namespace

std::optional<TreeIndex> index_tree(const char* data, std::size_t len, git_oid_t oid_type) {
  const std::size_t oid_raw = oid_type == GIT_OID_SHA1 ? 20 : 32;
  TreeIndex offsets;
  const char* p = data;
  const char* end = data + len;
  while (p < end) {
    const std::uint32_t off = static_cast<std::uint32_t>(p - data);
    std::uint32_t mode = 0;
    const char* q = parse_mode_prefix(p, end, &mode);
    if (!q) return std::nullopt;
    const void* nul = std::memchr(q, '\0', static_cast<std::size_t>(end - q));
    if (!nul) return std::nullopt;
    const char* name_end = static_cast<const char*>(nul);
    if (name_end == q) return std::nullopt;  // empty names are invalid
    if (end - (name_end + 1) < static_cast<long>(oid_raw)) return std::nullopt;  // truncated oid
    offsets.push_back(off);
    p = name_end + 1 + oid_raw;
  }
  return offsets;
}

std::optional<TreeEnt> entry_at(const TreeData& tree, std::uint32_t off, git_oid_t oid_type) {
  const std::size_t oid_raw = oid_type == GIT_OID_SHA1 ? 20 : 32;
  const char* data = tree.raw.data();
  const std::size_t len = tree.raw.size();
  if (off >= len) return std::nullopt;
  const char* p = data + off;
  const char* end = data + len;
  TreeEnt e;
  const char* q = parse_mode_prefix(p, end, &e.mode);
  if (!q) return std::nullopt;
  const void* nul = std::memchr(q, '\0', static_cast<std::size_t>(end - q));
  if (!nul) return std::nullopt;
  const char* name_end = static_cast<const char*>(nul);
  if (end - (name_end + 1) < static_cast<long>(oid_raw)) return std::nullopt;
  e.name = std::string_view(q, static_cast<std::size_t>(name_end - q));
  std::memcpy(e.oid.id, name_end + 1, oid_raw);
  return e;
}

int cmp_tree_name(std::string_view entry_name, bool entry_is_tree, std::string_view probe,
                  bool tree_slot) {
  const std::size_t n = std::min(entry_name.size(), probe.size());
  const int c = n ? std::memcmp(entry_name.data(), probe.data(), n) : 0;
  if (c != 0) return c;
  // The entry's stored sort key continues with '/' for trees, else ends.
  const char entry_next = entry_name.size() > n ? entry_name[n] : (entry_is_tree ? '/' : '\0');
  // The probe's effective key: "name/" for the directory-slot pass, the
  // bare name (virtual '\0') for the file pass. Using the slot key for a
  // file probe (0.0.3) sent the search right past file victims sitting
  // before their extension siblings ("at_file.c" vs "at_file.c.args");
  // using the bare key for a directory probe (0.0.2) missed directories
  // whose siblings extend them with bytes below '/' ("llvm-as" vs
  // "llvm-as-fuzzer", which sorts before the slot "llvm-as/").
  const char probe_next = probe.size() > n ? probe[n] : (tree_slot ? '/' : '\0');
  return static_cast<int>(static_cast<unsigned char>(entry_next)) -
         static_cast<int>(static_cast<unsigned char>(probe_next));
}

std::optional<TreeEnt> tree_find(const TreeData& tree, std::string_view name, git_oid_t oid_type) {
  if (tree.offsets.size() <= kLinearScanMax) {
    for (std::uint32_t off : tree.offsets) {
      auto e = entry_at(tree, off, oid_type);
      if (!e) return std::nullopt;
      if (e->name == name) return e;
    }
    return std::nullopt;
  }
  // Binary search over git's stored tree order. Trees written by git are
  // canonically sorted (the same assumption libgit2's byname lookup
  // makes); malformed hand-crafted trees simply miss, as before. Two
  // passes with the two probe keys (see cmp_tree_name) cover both victim
  // kinds; D/F uniqueness means at most one exact-name entry exists.
  for (bool tree_slot : {false, true}) {
    std::size_t lo = 0, hi = tree.offsets.size();
    while (lo < hi) {
      const std::size_t mid = lo + (hi - lo) / 2;
      auto e = entry_at(tree, tree.offsets[mid], oid_type);
      if (!e) return std::nullopt;
      const int c = cmp_tree_name(e->name, e->mode == kModeTree, name, tree_slot);
      if (c == 0) return e;
      if (c < 0)
        lo = mid + 1;
      else
        hi = mid;
    }
  }
  return std::nullopt;
}

std::optional<CommitFacts> parse_commit(const char* data, std::size_t len, git_oid_t oid_type) {
  CommitFacts out;
  bool have_tree = false, have_committer = false;
  const char* p = data;
  const char* end = data + len;

  while (p < end) {
    const char* nl =
        static_cast<const char*>(std::memchr(p, '\n', static_cast<std::size_t>(end - p)));
    const char* line_end = nl ? nl : end;
    const std::size_t line_len = static_cast<std::size_t>(line_end - p);
    std::string_view line(p, line_len);

    if (line.empty()) break;  // header block ends at the blank line
    if (line[0] == ' ') {     // continuation (gpgsig, ...) — skip
      p = nl ? nl + 1 : end;
      continue;
    }
    if (line.rfind("tree ", 0) == 0 && !have_tree) {
      if (oid_from_hex(p + 5, line_len - 5, oid_type, &out.root_tree)) have_tree = true;
    } else if (line.rfind("committer ", 0) == 0 && !have_committer) {
      // "committer Name <email> <ts> <tz>": the timestamp is the
      // second-to-last space-separated field.
      std::size_t last = line_len;
      for (int field = 0; field < 2; ++field) {
        if (last == 0) break;
        last = line.find_last_of(' ', last - 1);
        if (last == std::string_view::npos) break;
      }
      if (last != std::string_view::npos && last + 1 < line_len) {
        std::string_view ts = line.substr(last + 1);
        // strip a trailing tz if it glued on (shouldn't: tz is separated)
        std::size_t sp = ts.find(' ');
        if (sp != std::string_view::npos) ts = ts.substr(0, sp);
        if (!ts.empty() && ts.find_first_not_of("0123456789-") == std::string_view::npos) {
          out.committer_time = std::strtoll(std::string(ts).c_str(), nullptr, 10);
          have_committer = true;
        }
      }
    }
    p = nl ? nl + 1 : end;
  }
  if (!have_tree || !have_committer) return std::nullopt;
  return out;
}

std::optional<git_oid> parse_tag(const char* data, std::size_t len, git_oid_t oid_type) {
  const char* p = data;
  const char* end = data + len;
  while (p < end) {
    const char* nl =
        static_cast<const char*>(std::memchr(p, '\n', static_cast<std::size_t>(end - p)));
    const char* line_end = nl ? nl : end;
    const std::size_t line_len = static_cast<std::size_t>(line_end - p);
    if (line_len == 0) break;
    if (line_len > 7 && std::memcmp(p, "object ", 7) == 0) {
      git_oid out{};
      if (oid_from_hex(p + 7, line_len - 7, oid_type, &out)) return out;
      return std::nullopt;
    }
    p = nl ? nl + 1 : end;
  }
  return std::nullopt;
}

}  // namespace gitmount::rawobj
