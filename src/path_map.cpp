// gitmount — read-only git-to-FUSE filesystem (RFC 0000).
// SPDX-FileCopyrightText: 2026 The gitmount authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "path_map.hpp"

#include <cerrno>

namespace gitmount::pathmap {

namespace {

// Split on '/', dropping empty components ("/a//b/" → a, b).
std::vector<std::string> split_components(const std::string& path) {
  std::vector<std::string> out;
  std::size_t i = 0;
  while (i < path.size()) {
    std::size_t j = path.find('/', i);
    if (j == std::string::npos) j = path.size();
    if (j > i) out.emplace_back(path, i, j - i);
    i = j + 1;
  }
  return out;
}

}  // namespace

const char* ns_dir_name(Ns ns) {
  switch (ns) {
    case Ns::Branch:
      return "branch";
    case Ns::Tag:
      return "tag";
    case Ns::Remote:
      return "remote";
    case Ns::Commit:
      return "commit";
  }
  return "?";
}

bool is_full_oid(const std::string& s) {
  if (s.size() != 40 && s.size() != 64) return false;
  for (char c : s) {
    bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!ok) return false;
  }
  return true;
}

Parsed parse(const std::string& path) {
  Parsed p;
  if (path.empty() || path[0] != '/') {
    p.kind = Kind::Invalid;
    p.err = ENOENT;
    return p;
  }

  const std::vector<std::string> comps = split_components(path);

  if (comps.empty()) {
    p.kind = Kind::Root;
    return p;
  }

  // Component length gate: NAME_MAX (RFC 0000 §3.3). The kernel rejects
  // overlong lookups itself; this also guards readdir enumeration callers.
  for (const auto& c : comps) {
    if (c.size() > kNameMax) {
      p.kind = Kind::Invalid;
      p.err = ENAMETOOLONG;
      return p;
    }
  }

  const std::string& head = comps[0];
  if (head == "branch")
    p.ns = Ns::Branch;
  else if (head == "tag")
    p.ns = Ns::Tag;
  else if (head == "remote")
    p.ns = Ns::Remote;
  else if (head == "commit")
    p.ns = Ns::Commit;
  else if (head == "HEAD") {
    if (comps.size() == 1) {
      p.kind = Kind::HeadRoot;
      return p;
    }
    p.kind = Kind::HeadPath;
    p.comps.assign(comps.begin() + 1, comps.end());
    return p;
  } else if (head == "commits") {
    if (comps.size() == 1) {
      p.kind = Kind::CommitsFile;
      return p;
    }
    p.kind = Kind::Invalid;
    p.err = ENOTDIR;  // /commits/<x>: commits is a regular file
    return p;
  } else if (head == ".gitmount.json") {
    if (comps.size() == 1) {
      p.kind = Kind::MetaJson;
      return p;
    }
    p.kind = Kind::Invalid;
    p.err = ENOTDIR;
    return p;
  } else {
    p.kind = Kind::Invalid;
    p.err = ENOENT;
    return p;
  }

  if (comps.size() == 1) {
    p.kind = Kind::EntryDir;
    return p;
  }
  p.kind = Kind::NsPath;
  p.comps.assign(comps.begin() + 1, comps.end());
  return p;
}

}  // namespace gitmount::pathmap
