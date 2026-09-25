// gitmount — read-only git-to-FUSE filesystem (RFC 0000).
// SPDX-FileCopyrightText: 2026 The gitmount authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gitmount.hpp"

#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "log.hpp"
#include "path_map.hpp"

namespace gitmount {

namespace {

using pathmap::Kind;
using pathmap::Ns;
using pathmap::Parsed;

Gitmount* self() { return static_cast<Gitmount*>(fuse_get_context()->private_data); }

// First components of every ref name below `prefix` (e.g. refs/heads), deduped
// and byte-sorted (RFC 0000 §3.1: nested names render as grouping dirs and
// fold with same-named complete refs).
std::vector<std::string> first_components_under(const std::vector<std::string>& names,
                                                const std::string& prefix) {
  std::vector<std::string> out;
  const std::string p = prefix + "/";
  for (const auto& name : names) {
    if (name.size() <= p.size() || name.compare(0, p.size(), p) != 0) continue;
    const std::string rest = name.substr(p.size());
    const std::size_t slash = rest.find('/');
    out.push_back(slash == std::string::npos ? rest : rest.substr(0, slash));
  }
  std::sort(out.begin(), out.end(), byte_less);
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

// Structural UTF-8 sequence length at s[i]; 0 when invalid.
std::size_t utf8_seq_len(const std::string& s, std::size_t i) {
  const unsigned char c = static_cast<unsigned char>(s[i]);
  auto cont = [&](std::size_t j) {
    return j < s.size() && (static_cast<unsigned char>(s[j]) & 0xC0) == 0x80;
  };
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return cont(i + 1) ? 2 : 0;
  if ((c & 0xF0) == 0xE0) return (cont(i + 1) && cont(i + 2)) ? 3 : 0;
  if ((c & 0xF8) == 0xF0) return (cont(i + 1) && cont(i + 2) && cont(i + 3)) ? 4 : 0;
  return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

bool byte_less(const std::string& a, const std::string& b) {
  const std::size_t n = std::min(a.size(), b.size());
  const int c = n == 0 ? 0 : std::memcmp(a.data(), b.data(), n);
  if (c != 0) return c < 0;
  return a.size() < b.size();
}

std::string iso8601_utc(std::int64_t unix_time) {
  std::time_t t = static_cast<std::time_t>(unix_time);
  std::tm tmv{};
  gmtime_r(&t, &tmv);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
  return buf;
}

std::string json_quote_path(const std::string& s) {
  std::string out = "\"";
  std::size_t i = 0;
  char hex[8];
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == '"') {
      out += "\\\"";
      ++i;
      continue;
    }
    if (c == '\\') {
      out += "\\\\";
      ++i;
      continue;
    }
    if (c < 0x20) {
      std::snprintf(hex, sizeof(hex), "\\u%04x", c);
      out += hex;
      ++i;
      continue;
    }
    const std::size_t len = utf8_seq_len(s, i);
    bool valid = len != 0;
    if (valid) {
      const unsigned char c0 = c;
      const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
      if (len == 2 && c0 < 0xC2) valid = false;      // overlong
      if (len == 3 && ((c0 == 0xE0 && c1 < 0xA0) ||  // overlong
                       (c0 == 0xED && c1 >= 0xA0)))  // surrogate
        valid = false;
      if (len == 4 && c0 > 0xF4) valid = false;  // > U+10FFFF
    }
    if (valid) {
      out.append(s, i, len);
      i += len;
    } else {
      // Invalid UTF-8 byte: %XX escape (RFC 0000 §3.6). The escaping
      // guarantees valid JSON text; it is not reversible by contract.
      std::snprintf(hex, sizeof(hex), "%%%02X", c);
      out += hex;
      ++i;
    }
  }
  out += '"';
  return out;
}

// ---------------------------------------------------------------------------
// construction / .gitmount.json (RFC 0000 §3.6)
// ---------------------------------------------------------------------------

Gitmount::Gitmount(std::unique_ptr<GitRepo> repo, std::string repo_abs_path,
                   std::uint64_t blob_cache_bytes, std::uint64_t tree_cache_bytes)
    : repo_(std::move(repo)),
      repo_abs_path_(std::move(repo_abs_path)),
      blob_cache_bytes_(blob_cache_bytes),
      tree_cache_bytes_(tree_cache_bytes),
      blob_cache_(blob_cache_bytes) {
  mount_time_ = static_cast<std::int64_t>(::time(nullptr));
  uid_ = ::getuid();
  gid_ = ::getgid();

  const HeadInfo head = repo_->head();
  std::string head_json = "null";
  if (head.state == HeadInfo::State::Symbolic && !head.symbolic_target.empty())
    head_json = json_quote_path(head.symbolic_target);
  else if (head.state == HeadInfo::State::Detached)
    head_json = "\"" + oid_to_hex(head.oid) + "\"";

  json_content_ =
      "{\n"
      "  \"format\": 1,\n"
      "  \"repository\": " +
      json_quote_path(repo_abs_path_) +
      ",\n"
      "  \"head\": " +
      head_json +
      ",\n"
      "  \"mounted_at\": \"" +
      iso8601_utc(mount_time_) +
      "\",\n"
      "  \"cache\": { \"blob_bytes\": " +
      std::to_string(blob_cache_bytes_) + ", \"tree_bytes\": " + std::to_string(tree_cache_bytes_) +
      " }\n"
      "}\n";

  if (repo_->alternates_active())
    log::vlog(
        "repository uses alternates; external objects are not counted "
        "in statfs");
}

// ---------------------------------------------------------------------------
// resolution (RFC 0000 §3.1)
// ---------------------------------------------------------------------------

int Gitmount::walk_tree_locked(const git_oid& root_tree, const std::vector<std::string>& comps,
                               std::int64_t commit_time, Node* out) {
  git_oid cur = root_tree;
  out->commit_root_tree = root_tree;
  std::string walked;  // path inside the commit tree ("" at the root)
  for (std::size_t i = 0; i < comps.size(); ++i) {
    const bool last = (i + 1 == comps.size());
    const std::string& name = comps[i];

    auto entry = repo_->tree_entry(cur, name);
    if (!entry) {
      // Submodule marker fallback: "<name>.gitmount-submodule" with no real
      // entry of that name and a gitlink base entry (RFC 0000 §3.2). A real
      // entry always wins (checked above by the failed lookup).
      constexpr char kSuffix[] = ".gitmount-submodule";
      constexpr std::size_t kSuffixLen = sizeof(kSuffix) - 1;
      if (name.size() > kSuffixLen &&
          name.compare(name.size() - kSuffixLen, kSuffixLen, kSuffix) == 0) {
        const std::string base = name.substr(0, name.size() - kSuffixLen);
        auto base_entry = repo_->tree_entry(cur, base);
        if (base_entry && base_entry->mode == GIT_FILEMODE_COMMIT && last) {
          out->type = Node::Type::SubmoduleMarker;
          // .gitmodules keys submodules by their full tree path (§3.2).
          out->marker_path = walked.empty() ? base : walked + "/" + base;
          out->gitlink_oid = base_entry->oid;
          out->commit_time = commit_time;
          return 0;
        }
      }
      return -ENOENT;
    }
    switch (entry->mode) {
      case GIT_FILEMODE_TREE:
        if (last) {
          out->type = Node::Type::TreeDir;
          out->tree_oid = entry->oid;
          out->commit_time = commit_time;
          return 0;
        }
        walked = walked.empty() ? name : walked + "/" + name;
        cur = entry->oid;
        break;
      case GIT_FILEMODE_BLOB:
      case GIT_FILEMODE_BLOB_EXECUTABLE:
        if (!last) return -ENOENT;
        out->type = Node::Type::BlobFile;
        out->blob_oid = entry->oid;
        out->executable = entry->mode == GIT_FILEMODE_BLOB_EXECUTABLE;
        out->commit_time = commit_time;
        return 0;
      case GIT_FILEMODE_LINK:
        if (!last) return -ENOENT;
        out->type = Node::Type::Symlink;
        out->blob_oid = entry->oid;
        out->commit_time = commit_time;
        return 0;
      case GIT_FILEMODE_COMMIT:
        if (!last) return -ENOENT;  // cannot descend into a gitlink
        out->type = Node::Type::SubmoduleDir;
        out->commit_time = commit_time;
        return 0;
      default:
        return -EIO;  // garbage mode in a hand-crafted tree
    }
  }
  out->type = Node::Type::TreeDir;
  out->tree_oid = cur;
  out->commit_time = commit_time;
  return 0;
}

namespace {

// Peel a ref target to its commit's root tree + committer time. Returns 0
// or -ENOENT (+warning) for non-commit targets (RFC 0000 §3.1 unified rule).
int peel_ref_to_root(GitRepo* repo, const git_oid& target, const std::string& refname_for_log,
                     git_oid* root_out, std::int64_t* time_out) {
  git_oid commit;
  if (repo->peel_to_commit(target, &commit) != 0) {
    log::warn("ref %s does not peel to a commit; entry not presented", refname_for_log.c_str());
    return -ENOENT;
  }
  repo->commit_committer_time(commit, time_out);
  return repo->commit_root_tree(commit, root_out);
}

}  // namespace

int Gitmount::resolve_head_locked(const std::vector<std::string>& tree_comps, Node* out) {
  const HeadInfo head = repo_->head();
  if (head.state == HeadInfo::State::Unborn) return -ENOENT;

  git_oid target;
  if (head.state == HeadInfo::State::Detached) {
    target = head.oid;
  } else {
    auto ref = repo_->lookup_ref(head.symbolic_target);
    if (!ref) return -ENOENT;
    target = ref->target;
  }

  git_oid root;
  std::int64_t when = 0;
  const int rc = peel_ref_to_root(repo_.get(), target, "HEAD", &root, &when);
  if (rc != 0) return rc;
  return walk_tree_locked(root, tree_comps, when, out);
}

int Gitmount::resolve_ns_locked(const std::string& ref_ns_prefix,
                                const std::vector<std::string>& comps, Node* out) {
  // Longest ref name match first (RFC 0000 §3.1): in D/F-violating repos
  // (hand-edited packed-refs) /tag/foo/bar resolves to refs/tags/foo/bar,
  // never to ref foo's tree entry "bar".
  std::string joined;
  for (std::size_t k = comps.size(); k >= 1; --k) {
    joined.clear();
    for (std::size_t i = 0; i < k; ++i) {
      if (i) joined += '/';
      joined += comps[i];
    }
    const std::string refname = ref_ns_prefix + "/" + joined;
    auto info = repo_->lookup_ref(refname);
    if (!info) continue;

    git_oid root;
    std::int64_t when = 0;
    const int rc = peel_ref_to_root(repo_.get(), info->target, refname, &root, &when);
    if (rc != 0) return rc;
    out->ref_prefix = refname;  // merged nodes union their sub-refs (readdir)

    // Folding ambiguity defense (RFC 0000 §3.1): a shorter ref match means
    // the path also had a parent-ref + tree-entry interpretation.
    if (k > 1) {
      for (std::size_t kk = k - 1; kk >= 1; --kk) {
        std::string shorter;
        for (std::size_t i = 0; i < kk; ++i) {
          if (i) shorter += '/';
          shorter += comps[i];
        }
        if (repo_->lookup_ref(ref_ns_prefix + "/" + shorter)) {
          log::warn(
              "folded ambiguity on '%s': resolved by longest ref name "
              "match (%s)",
              joined.c_str(), refname.c_str());
          break;
        }
      }
    }
    return walk_tree_locked(root, std::vector<std::string>(comps.begin() + k, comps.end()), when,
                            out);
  }

  // No ref matched: group-prefix node when sub-refs exist below the path,
  // else ENOENT (RFC 0000 §3.1).
  std::string joined_all;
  for (std::size_t i = 0; i < comps.size(); ++i) {
    if (i) joined_all += '/';
    joined_all += comps[i];
  }
  const std::string group_prefix = ref_ns_prefix + "/" + joined_all;
  if (!repo_->refs_under(group_prefix).empty()) {
    out->type = Node::Type::GroupDir;
    out->group_prefix = group_prefix;
    return 0;
  }
  return -ENOENT;
}

int Gitmount::resolve_remote_locked(const std::vector<std::string>& comps, Node* out) {
  const std::string ns = comps[0];
  const std::string ns_prefix = "refs/remotes/" + ns;

  // Hidden HEAD (RFC 0000 §3.1): symbolic refs/remotes/<ns>/HEAD is skipped
  // in lookup as well as enumeration — the whole /remote/<ns>/HEAD path.
  if (comps.size() >= 2 && comps[1] == "HEAD") {
    const std::string head_ref = ns_prefix + "/HEAD";
    auto info = repo_->lookup_ref(head_ref);
    if (info && info->symbolic) return -ENOENT;
    // Non-symbolic HEAD (update-ref direct write) is an ordinary tracking
    // ref and falls through to the generic resolution below.
  }

  const bool bare_exists = repo_->lookup_ref(ns_prefix).has_value();
  const bool has_children = !repo_->refs_under(ns_prefix).empty();

  if (comps.size() == 1) {
    if (bare_exists && has_children) {
      // Merged namespace node: the bare ref is reachable again (§3.1).
      auto info = repo_->lookup_ref(ns_prefix);
      git_oid root;
      std::int64_t when = 0;
      const int rc = peel_ref_to_root(repo_.get(), info->target, ns_prefix, &root, &when);
      if (rc != 0) return rc;
      out->ref_prefix = ns_prefix;
      return walk_tree_locked(root, {}, when, out);
    }
    // Bare namespace ref (unreachable itself) or a plain namespace dir.
    if (bare_exists || has_children) {
      out->type = Node::Type::GroupDir;
      out->group_prefix = ns_prefix;
      return 0;
    }
    return -ENOENT;
  }

  // Generic longest-ref match from full length down to the two-component
  // split <remote>/<branch-path> (RFC 0000 §3.1).
  std::string joined;
  for (std::size_t k = comps.size(); k >= 2; --k) {
    joined.clear();
    for (std::size_t i = 0; i < k; ++i) {
      if (i) joined += '/';
      joined += comps[i];
    }
    const std::string refname = "refs/remotes/" + joined;
    auto info = repo_->lookup_ref(refname);
    if (!info) continue;

    git_oid root;
    std::int64_t when = 0;
    const int rc = peel_ref_to_root(repo_.get(), info->target, refname, &root, &when);
    if (rc != 0) return rc;
    out->ref_prefix = refname;
    return walk_tree_locked(root, std::vector<std::string>(comps.begin() + k, comps.end()), when,
                            out);
  }

  // k = 1: the namespace itself as a merged node (bare ref WITH children);
  // a bare ref without children is unreachable below the namespace (§3.1).
  if (bare_exists && has_children) {
    auto info = repo_->lookup_ref(ns_prefix);
    git_oid root;
    std::int64_t when = 0;
    const int rc = peel_ref_to_root(repo_.get(), info->target, ns_prefix, &root, &when);
    if (rc != 0) return rc;
    out->ref_prefix = ns_prefix;
    return walk_tree_locked(root, std::vector<std::string>(comps.begin() + 1, comps.end()), when,
                            out);
  }

  // Deeper group-prefix directory (e.g. /remote/origin/feature when only
  // refs/remotes/origin/feature/* exist).
  std::string joined_all;
  for (std::size_t i = 0; i < comps.size(); ++i) {
    if (i) joined_all += '/';
    joined_all += comps[i];
  }
  const std::string group_prefix = "refs/remotes/" + joined_all;
  if (!repo_->refs_under(group_prefix).empty()) {
    out->type = Node::Type::GroupDir;
    out->group_prefix = group_prefix;
    return 0;
  }
  return -ENOENT;
}

int Gitmount::resolve_locked(const std::string& path, Node* out) {
  *out = Node{};
  const Parsed p = pathmap::parse(path);
  if (p.kind == Kind::Invalid) return -p.err;

  switch (p.kind) {
    case Kind::Invalid:
      break;
    case Kind::Root:
      out->type = Node::Type::RootDir;
      return 0;
    case Kind::EntryDir:
      out->type = Node::Type::EntryDir;
      out->ns = p.ns;
      return 0;
    case Kind::HeadRoot:
      return resolve_head_locked({}, out);
    case Kind::HeadPath:
      return resolve_head_locked(p.comps, out);
    case Kind::CommitsFile:
      out->type = Node::Type::CommitsFile;
      return 0;
    case Kind::MetaJson:
      out->type = Node::Type::JsonFile;
      return 0;
    case Kind::NsPath:
      switch (p.ns) {
        case Ns::Branch:
          return resolve_ns_locked("refs/heads", p.comps, out);
        case Ns::Tag:
          return resolve_ns_locked("refs/tags", p.comps, out);
        case Ns::Remote:
          return resolve_remote_locked(p.comps, out);
        case Ns::Commit: {
          // Full lowercase oid only, pointing at a commit object itself —
          // no peeling, replace refs never honored (RFC 0000 §3.1).
          if (!pathmap::is_full_oid(p.comps[0])) return -ENOENT;
          git_oid oid{};
          if (git_oid_fromstr(&oid, p.comps[0].c_str()) != 0) return -ENOENT;
          if (!repo_->is_commit_object(oid)) return -ENOENT;
          std::int64_t when = 0;
          repo_->commit_committer_time(oid, &when);
          git_oid root;
          if (repo_->commit_root_tree(oid, &root) != 0) return -ENOENT;
          return walk_tree_locked(
              root, std::vector<std::string>(p.comps.begin() + 1, p.comps.end()), when, out);
        }
      }
      return -ENOENT;
  }
  return -ENOENT;
}

// ---------------------------------------------------------------------------
// readdir enumeration
// ---------------------------------------------------------------------------

void Gitmount::append_tree_entries_locked(const git_oid& tree,
                                          const std::string& ref_prefix_for_union,
                                          std::vector<DirEntry>* out) {
  auto entries = repo_->tree_entries(tree);
  if (!entries) return;

  for (const auto& e : *entries) {
    // Hand-crafted trees may contain "." / ".."; skip (kernel synthesizes
    // them) and warn (RFC 0000 §3.1). Overlong names are skipped likewise.
    if (e.name == "." || e.name == "..") {
      log::warn("skipping pathological tree entry '%s' (reserved name)", e.name.c_str());
      continue;
    }
    if (e.name.size() > pathmap::kNameMax) {
      log::warn("skipping overlong tree entry (%zu bytes > NAME_MAX)", e.name.size());
      continue;
    }
    DirEntry de;
    de.name = e.name;
    switch (e.mode) {
      case GIT_FILEMODE_TREE:
        de.mode = S_IFDIR;
        break;
      case GIT_FILEMODE_BLOB:
      case GIT_FILEMODE_BLOB_EXECUTABLE:
        de.mode = S_IFREG;
        break;
      case GIT_FILEMODE_LINK:
        de.mode = S_IFLNK;
        break;
      case GIT_FILEMODE_COMMIT: {
        // Submodule: empty directory + marker file; a real entry shadowing
        // the marker name wins and the synthetic file is omitted (§3.2).
        de.mode = S_IFDIR;
        out->push_back(de);
        const std::string marker = e.name + ".gitmount-submodule";
        bool shadowed = false;
        for (const auto& other : *entries) {
          if (other.name == marker) {
            shadowed = true;
            break;
          }
        }
        if (shadowed) {
          log::warn("tree entry '%s' shadows synthetic submodule marker", marker.c_str());
        } else if (marker.size() <= pathmap::kNameMax) {
          out->push_back(DirEntry{marker, S_IFREG});
        }
        continue;
      }
      default:
        de.mode = S_IFREG;  // unreadable mode in a hand-crafted tree
        break;
    }
    out->push_back(de);
  }

  // Merged-node union (RFC 0000 §3.1): root tree entries ∪ first components
  // of sub-ref names, mixed into one byte-dictionary order; same-name entries
  // fold to the sub-ref (directory) rendering with a warning.
  if (!ref_prefix_for_union.empty()) {
    for (const auto& first : first_components_under(repo_->all_refs(), ref_prefix_for_union)) {
      bool folded = false;
      for (auto& de : *out) {
        if (de.name == first) {
          if (de.mode != S_IFDIR) {
            log::warn(
                "folded ambiguity: tree entry '%s' and sub-ref render "
                "as directory (sub-ref wins)",
                first.c_str());
          }
          de.mode = S_IFDIR;
          folded = true;
          break;
        }
      }
      if (!folded) out->push_back(DirEntry{first, S_IFDIR});
    }
  }

  std::sort(out->begin(), out->end(),
            [](const DirEntry& a, const DirEntry& b) { return byte_less(a.name, b.name); });
}

int Gitmount::list_dir_locked(const std::string&, const Node& node, std::vector<DirEntry>* out) {
  switch (node.type) {
    case Node::Type::RootDir:
      *out = {{".gitmount.json", S_IFREG},
              {"HEAD", S_IFDIR},
              {"branch", S_IFDIR},
              {"commit", S_IFDIR},
              {"commits", S_IFREG},
              {"remote", S_IFDIR},
              {"tag", S_IFDIR}};
      return 0;
    case Node::Type::EntryDir: {
      const char* want = nullptr;
      switch (node.ns) {
        case Ns::Branch:
          want = "refs/heads";
          break;
        case Ns::Tag:
          want = "refs/tags";
          break;
        case Ns::Remote:
          want = "refs/remotes";
          break;
        case Ns::Commit:
          return 0;  // /commit readdir is permanently empty (§3.1)
      }
      for (const auto& first : first_components_under(repo_->all_refs(), want))
        out->push_back(DirEntry{first, S_IFDIR});
      return 0;
    }
    case Node::Type::GroupDir: {
      for (const auto& first : first_components_under(repo_->all_refs(), node.group_prefix))
        out->push_back(DirEntry{first, S_IFDIR});
      // Hidden HEAD exclusion: symbolic refs/remotes/<ns>/HEAD never shows
      // in namespace listings (RFC 0000 §3.1) — only at the namespace level.
      const std::string& gp = node.group_prefix;
      if (gp.rfind("refs/remotes/", 0) == 0) {
        const std::string rest = gp.substr(strlen("refs/remotes/"));
        if (rest.find('/') == std::string::npos) {  // gp == refs/remotes/<ns>
          auto info = repo_->lookup_ref(gp + "/HEAD");
          if (info && info->symbolic) {
            for (auto it = out->begin(); it != out->end(); ++it) {
              if (it->name == "HEAD") {
                out->erase(it);
                break;
              }
            }
          }
        }
      }
      return 0;
    }
    case Node::Type::TreeDir:
      append_tree_entries_locked(node.tree_oid, node.ref_prefix, out);
      return 0;
    case Node::Type::SubmoduleDir:
      return 0;  // submodules render as empty directories (§3.2)
    default:
      return -ENOTDIR;
  }
}

// ---------------------------------------------------------------------------
// blob access (segmented locking, RFC 0000 §3.4/§3.5)
// ---------------------------------------------------------------------------

int Gitmount::decompress_blob_unlocked(const git_oid& oid, std::string* out) {
  const int rc = repo_->read_blob(oid, out);
  if (rc != 0) return rc;
  log::vlog("blob decompressed (oversized open-pin): %s %zu bytes", oid_to_hex(oid).c_str(),
            out->size());
  return 0;
}

int Gitmount::load_blob_locked(const git_oid& oid, BlobView* view) {
  const std::string key = oid_to_hex(oid);
  if (const std::string* hit = blob_cache_.lookup(key)) {
    log::vlog("blob cache hit: %s", key.c_str());
    view->cached = hit;
    view->owned.clear();
    return 0;
  }

  // Miss: decompress outside the lock (§3.4 exception 3).
  mu_.unlock();
  std::string data;
  const int rc = repo_->read_blob(oid, &data);
  if (rc == 0)
    log::vlog("blob decompressed (cache miss load): %s %zu bytes", key.c_str(), data.size());
  mu_.lock();
  if (rc != 0) return rc;

  // Re-check: a concurrent loader may have won the race (no strict
  // single-flight for cacheable blobs — §3.4).
  if (const std::string* hit = blob_cache_.lookup(key)) {
    view->cached = hit;
    view->owned.clear();
    return 0;
  }
  if (data.size() < blob_cache_.capacity()) {
    // Insert by move: the payload is allocated exactly once (the ODB
    // read); the previous by-lvalue insert copied the whole blob on
    // every miss load (stress-test finding ② — transient double
    // buffering under concurrent readers).
    blob_cache_.insert(key, std::move(data));
    view->cached = blob_cache_.lookup(key);
    view->owned.clear();
    view->owned.shrink_to_fit();
    return 0;
  }
  // Bypass: values at/over capacity stay private to this call (§3.5).
  view->cached = nullptr;
  view->owned = std::move(data);
  return 0;
}

// ---------------------------------------------------------------------------
// /commits (RFC 0000 §3.5)
// ---------------------------------------------------------------------------

std::string Gitmount::compute_fingerprint_locked() {
  auto names = repo_->all_refs();
  std::sort(names.begin(), names.end(), byte_less);
  std::string fp;
  for (const auto& name : names) {
    auto info = repo_->lookup_ref(name);
    if (!info) continue;
    fp += name;
    fp.push_back('\0');
    fp += oid_to_hex(info->target);
    fp.push_back('\n');
  }
  fp += "HEAD\0";
  const HeadInfo head = repo_->head();
  if (head.state == HeadInfo::State::Detached) {
    fp += oid_to_hex(head.oid);
  } else if (head.state == HeadInfo::State::Symbolic) {
    auto ref = repo_->lookup_ref(head.symbolic_target);
    fp += ref ? oid_to_hex(ref->target) : "-";
  } else {
    fp += "-";
  }
  fp.push_back('\n');
  return fp;
}

int Gitmount::open_commits(std::shared_ptr<const std::string>* out) {
  std::unique_lock<std::mutex> g(commits_gen_mu_);

  // Fast path under mu_.
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (commits_buffer_ && commits_fingerprint_ == compute_fingerprint_locked()) {
      *out = commits_buffer_;
      return 0;
    }
  }

  if (commits_generating_) {
    // Single-flight: wait for the in-flight generation and share its
    // outcome — success reuses the buffer, failure maps to EIO (§3.4/§3.5).
    const std::uint64_t epoch = commits_epoch_;
    commits_cv_.wait(g, [&] { return commits_epoch_ != epoch; });
    std::lock_guard<std::mutex> lk(mu_);
    if (commits_buffer_) {
      *out = commits_buffer_;
      return 0;
    }
    return -EIO;
  }

  // Double-check after acquiring the generation lock.
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (commits_buffer_ && commits_fingerprint_ == compute_fingerprint_locked()) {
      *out = commits_buffer_;
      return 0;
    }
  }

  commits_generating_ = true;
  g.unlock();

  // ---- generation (chunked locking inside list_commits) ------------------
  std::string fingerprint;
  std::string buffer;
  bool ok = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    fingerprint = compute_fingerprint_locked();

    // Push set: every ref under refs/ (symbolic refs resolved), each peeled
    // to a commit — peel failures skip with a verbose note, never an error;
    // HEAD is pushed last. Determinism: ref names byte-sorted for push
    // order + GIT_SORT_TOPOLOGICAL (RFC 0000 §3.5).
    auto names = repo_->all_refs();
    std::sort(names.begin(), names.end(), byte_less);
    std::vector<git_oid> pushes;
    for (const auto& name : names) {
      auto info = repo_->lookup_ref(name);
      if (!info) continue;
      git_oid commit;
      if (repo_->peel_to_commit(info->target, &commit) != 0) {
        log::vlog("commits list: skipping non-commit ref %s", name.c_str());
        continue;
      }
      pushes.push_back(commit);
    }
    const HeadInfo head = repo_->head();
    if (head.state != HeadInfo::State::Unborn) {
      git_oid target = head.oid;
      if (head.state == HeadInfo::State::Symbolic) {
        auto ref = repo_->lookup_ref(head.symbolic_target);
        if (ref) target = ref->target;
      }
      git_oid commit;
      if (repo_->peel_to_commit(target, &commit) == 0)
        pushes.push_back(commit);
      else
        log::vlog("commits list: HEAD does not peel to a commit, skipped");
    }

    ok =
        repo_->list_commits(pushes, [this] { mu_.unlock(); }, [this] { mu_.lock(); }, &buffer) == 0;
  }

  g.lock();
  commits_generating_ = false;
  ++commits_epoch_;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (ok) {
      commits_buffer_ = std::make_shared<const std::string>(std::move(buffer));
      commits_fingerprint_ = fingerprint;
      commits_generated_at_ = static_cast<std::int64_t>(::time(nullptr));
      *out = commits_buffer_;
    }
    // Failure: half-built buffer dropped, fingerprint untouched — "as if
    // never generated"; the next open retries from scratch (§3.5).
  }
  commits_cv_.notify_all();
  return ok ? 0 : -EIO;
}

// ---------------------------------------------------------------------------
// submodule markers (RFC 0000 §3.2)
// ---------------------------------------------------------------------------

std::string Gitmount::submodule_marker_content_locked(const git_oid& commit_tree,
                                                      const std::string& name,
                                                      const git_oid& gitlink_oid) {
  std::string url;
  auto root = repo_->tree_entries(commit_tree);
  const git_oid* modules_oid = nullptr;
  if (root) {
    for (const auto& e : *root) {
      if (e.name == ".gitmodules" &&
          (e.mode == GIT_FILEMODE_BLOB || e.mode == GIT_FILEMODE_BLOB_EXECUTABLE)) {
        modules_oid = &e.oid;
        break;
      }
    }
  }
  if (!modules_oid) {
    log::warn("no .gitmodules at commit root for submodule '%s'", name.c_str());
  } else {
    // .gitmodules goes through the ordinary LRU blob path (§3.2/§3.5):
    // hits are pure memory; misses (and pathological oversize bypasses)
    // decompress and log once per call.
    BlobView view;
    if (load_blob_locked(*modules_oid, &view) == 0) {
      for (const auto& [path, u] : parse_gitmodules(std::string(view.data(), view.size()))) {
        if (path == name) {
          url = u;
          break;
        }
      }
      if (url.empty()) log::warn(".gitmodules has no entry for submodule '%s'", name.c_str());
    }
  }
  return "url=" + url + "\ncommit=" + oid_to_hex(gitlink_oid) + "\n";
}

// ---------------------------------------------------------------------------
// metadata helpers (RFC 0000 §3.2)
// ---------------------------------------------------------------------------

void Gitmount::fill_dir_stat(struct stat* st, std::int64_t mtime) {
  std::memset(st, 0, sizeof(*st));
  st->st_mode = S_IFDIR | 0755;
  st->st_nlink = 2;
  st->st_size = 4096;  // universal directory st_size (§3.2)
  st->st_blksize = 4096;
  st->st_blocks = (4096 + 511) / 512;
  st->st_uid = uid_;
  st->st_gid = gid_;
  st->st_atime = st->st_mtime = st->st_ctime = static_cast<time_t>(mtime);
}

void Gitmount::fill_file_stat(struct stat* st, std::int64_t mtime, std::uint64_t size,
                              mode_t perm) {
  std::memset(st, 0, sizeof(*st));
  st->st_mode = perm;
  st->st_nlink = 1;
  st->st_size = static_cast<off_t>(size);
  st->st_blksize = 4096;
  st->st_blocks = (size + 511) / 512;
  st->st_uid = uid_;
  st->st_gid = gid_;
  st->st_atime = st->st_mtime = st->st_ctime = static_cast<time_t>(mtime);
}

// ---------------------------------------------------------------------------
// FUSE callbacks
// ---------------------------------------------------------------------------

int Gitmount::cb_getattr(const char* path_c, struct stat* st, struct fuse_file_info*) {
  Gitmount* fs = self();
  const std::string path(path_c);
  std::lock_guard<std::mutex> lk(fs->mu_);

  Node node;
  const int rc = fs->resolve_locked(path, &node);
  if (rc != 0) return rc;

  switch (node.type) {
    case Node::Type::RootDir:
    case Node::Type::EntryDir:
    case Node::Type::GroupDir:
      fs->fill_dir_stat(st, fs->mount_time_);  // mount-time stamps (§3.2)
      break;
    case Node::Type::TreeDir:
    case Node::Type::SubmoduleDir:
      fs->fill_dir_stat(st, node.commit_time);
      break;
    case Node::Type::BlobFile: {
      // Header-only size: no full decompression for ordinary blobs and no
      // revwalk here (RFC 0000 §3.2/§3.3).
      std::uint64_t size = 0;
      const int rc2 = fs->repo_->blob_size(node.blob_oid, &size);
      if (rc2 != 0) return -rc2;
      fs->fill_file_stat(st, node.commit_time, size, S_IFREG | (node.executable ? 0755u : 0644u));
      break;
    }
    case Node::Type::Symlink: {
      // Explicit exception (§3.2): st_size is the truncation-semantics
      // length — the first NUL locates the truncation point.
      BlobView view;
      const int rc2 = fs->load_blob_locked(node.blob_oid, &view);
      if (rc2 != 0) return -rc2;
      const void* nulp = std::memchr(view.data(), '\0', view.size());
      const std::uint64_t len =
          nulp ? static_cast<std::uint64_t>(static_cast<const char*>(nulp) - view.data())
               : view.size();
      fs->fill_file_stat(st, node.commit_time, len, S_IFLNK | 0777);
      break;
    }
    case Node::Type::SubmoduleMarker: {
      const std::string content = fs->submodule_marker_content_locked(
          node.commit_root_tree, node.marker_path, node.gitlink_oid);
      fs->fill_file_stat(st, node.commit_time, content.size(), S_IFREG | 0644);
      break;
    }
    case Node::Type::CommitsFile: {
      // Pure cache read: never triggers revwalk or fingerprint recompute;
      // st_size = 0 before first generation (RFC 0000 §3.3/§3.5).
      const std::uint64_t size = fs->commits_buffer_ ? fs->commits_buffer_->size() : 0;
      const std::int64_t mtime = fs->commits_buffer_ ? fs->commits_generated_at_ : fs->mount_time_;
      fs->fill_file_stat(st, mtime, size, S_IFREG | 0444);
      break;
    }
    case Node::Type::JsonFile:
      fs->fill_file_stat(st, fs->mount_time_, fs->json_content_.size(), S_IFREG | 0444);
      break;
  }

  st->st_ino = fs->inos_.register_path(path);
  return 0;
}

int Gitmount::cb_readlink(const char* path_c, char* buf, std::size_t size) {
  Gitmount* fs = self();
  std::lock_guard<std::mutex> lk(fs->mu_);

  Node node;
  const int rc = fs->resolve_locked(path_c, &node);
  if (rc != 0) return rc;
  if (node.type != Node::Type::Symlink) return -EINVAL;

  BlobView view;
  const int rc2 = fs->load_blob_locked(node.blob_oid, &view);
  if (rc2 != 0) return -rc2;

  // Truncate at the first NUL (kernel targets cannot contain NUL); an empty
  // blob is an empty target; truncation length >= PATH_MAX is ENAMETOOLONG
  // (boundary pinned to >= in RFC 0000 §3.3).
  const char* data = view.data();
  std::size_t len = view.size();
  const void* nulp = std::memchr(data, '\0', view.size());
  if (nulp) len = static_cast<std::size_t>(static_cast<const char*>(nulp) - data);
  if (len >= PATH_MAX) return -ENAMETOOLONG;
  if (size == 0) return -EINVAL;
  const std::size_t copy = std::min(len, size - 1);
  std::memcpy(buf, data, copy);
  buf[copy] = '\0';
  return 0;
}

int Gitmount::cb_opendir(const char* path_c, struct fuse_file_info* fi) {
  Gitmount* fs = self();
  const std::string path(path_c);
  std::lock_guard<std::mutex> lk(fs->mu_);

  Node node;
  const int rc = fs->resolve_locked(path, &node);
  if (rc != 0) return rc;

  std::vector<DirEntry> entries;
  if (fs->list_dir_locked(path, node, &entries) != 0) return -ENOTDIR;

  // Register inodes for every listed entry (lazy registration on readdir,
  // RFC 0000 §3.2) and pin the snapshot on fi->fh for stable offsets within
  // this directory stream (§3.3).
  std::string parent = path == "/" ? "" : path;
  for (auto& e : entries) {
    const std::string child = parent + "/" + e.name;
    e.ino = fs->inos_.register_path(child);
  }

  fi->fh = reinterpret_cast<std::uint64_t>(new DirList{std::move(entries)});
  return 0;
}

int Gitmount::cb_readdir(const char*, void* buf, fuse_fill_dir_t filler, off_t offset,
                         struct fuse_file_info* fi, enum fuse_readdir_flags) {
  auto* list = reinterpret_cast<DirList*>(fi->fh);
  if (!list) return -EINVAL;
  // Plain filler stats (ino + type): full attributes stay with getattr —
  // FUSE_FILL_DIR_PLUS is deliberately not claimed with partial stats.
  for (std::size_t i = static_cast<std::size_t>(offset); i < list->entries.size(); ++i) {
    struct stat st {};
    st.st_ino = list->entries[i].ino;
    st.st_mode = list->entries[i].mode;
    if (filler(buf, list->entries[i].name.c_str(), &st, static_cast<off_t>(i + 1),
               static_cast<enum fuse_fill_dir_flags>(0)))
      break;
  }
  return 0;
}

int Gitmount::cb_releasedir(const char*, struct fuse_file_info* fi) {
  delete reinterpret_cast<DirList*>(fi->fh);
  fi->fh = 0;
  return 0;
}

int Gitmount::cb_open(const char* path_c, struct fuse_file_info* fi) {
  Gitmount* fs = self();
  const std::string path(path_c);

  // Only O_RDONLY opens are valid; write flags get EROFS (RFC 0000 §3.3).
  if ((fi->flags & O_ACCMODE) != O_RDONLY) return -EROFS;

  std::lock_guard<std::mutex> lk(fs->mu_);
  Node node;
  const int rc = fs->resolve_locked(path, &node);
  if (rc != 0) return rc;

  auto* handle = new Handle();
  switch (node.type) {
    case Node::Type::BlobFile: {
      // Oversized admission via header-only size (boundary is >=, §3.5):
      // decompress once OUTSIDE the lock and pin on this handle — every
      // read of this open slices the pinned block (§3.3/§3.5).
      std::uint64_t size = 0;
      const int rc2 = fs->repo_->blob_size(node.blob_oid, &size);
      if (rc2 != 0) {
        delete handle;
        return -rc2;
      }
      if (size >= fs->blob_cache_.capacity()) {
        fs->mu_.unlock();
        std::string data;
        const int rc3 = fs->decompress_blob_unlocked(node.blob_oid, &data);
        fs->mu_.lock();
        if (rc3 != 0) {
          delete handle;
          return -rc3;
        }
        handle->kind = Handle::Kind::PinnedBlob;
        handle->data = std::move(data);
      } else {
        handle->kind = Handle::Kind::Plain;
      }
      break;
    }
    case Node::Type::Symlink:
      handle->kind = Handle::Kind::Plain;  // readlink is the access path
      break;
    case Node::Type::CommitsFile: {
      // First open triggers generation; the buffer version is pinned on
      // this handle until release (RFC 0000 §3.3/§3.5). open_commits takes
      // its own (outer) generation lock — mu_ is released while it runs.
      fs->mu_.unlock();
      std::shared_ptr<const std::string> buffer;
      const int rc2 = fs->open_commits(&buffer);
      fs->mu_.lock();
      if (rc2 != 0) {
        delete handle;
        return rc2;
      }
      handle->kind = Handle::Kind::PinnedCommits;
      handle->shared = std::move(buffer);
      break;
    }
    case Node::Type::JsonFile:
      // Immutable for the mount: no pinning needed (§3.6).
      handle->kind = Handle::Kind::Synthetic;
      handle->shared = std::make_shared<const std::string>(fs->json_content_);
      break;
    case Node::Type::SubmoduleMarker:
      handle->kind = Handle::Kind::Synthetic;
      handle->shared = std::make_shared<const std::string>(fs->submodule_marker_content_locked(
          node.commit_root_tree, node.marker_path, node.gitlink_oid));
      break;
    case Node::Type::RootDir:
    case Node::Type::EntryDir:
    case Node::Type::GroupDir:
    case Node::Type::TreeDir:
    case Node::Type::SubmoduleDir:
      handle->kind = Handle::Kind::Dir;
      break;
  }
  fi->fh = reinterpret_cast<std::uint64_t>(handle);
  return 0;
}

int Gitmount::cb_read(const char* path_c, char* buf, std::size_t size, off_t offset,
                      struct fuse_file_info* fi) {
  Gitmount* fs = self();
  auto* handle = reinterpret_cast<Handle*>(fi->fh);
  if (!handle) return -EINVAL;

  const char* data = nullptr;
  std::size_t len = 0;
  BlobView view;  // keeps a cache hit alive while copying

  switch (handle->kind) {
    case Handle::Kind::PinnedBlob:
      data = handle->data.data();
      len = handle->data.size();
      break;
    case Handle::Kind::PinnedCommits:
    case Handle::Kind::Synthetic:
      data = handle->shared->data();
      len = handle->shared->size();
      break;
    case Handle::Kind::Plain: {
      // Re-resolve per read; cacheable blobs go through the LRU (§3.5).
      std::lock_guard<std::mutex> lk(fs->mu_);
      Node node;
      const int rc = fs->resolve_locked(path_c, &node);
      if (rc != 0) return rc;
      if (node.type == Node::Type::Symlink) return -EINVAL;
      if (node.type != Node::Type::BlobFile) return -EISDIR;
      const int rc2 = fs->load_blob_locked(node.blob_oid, &view);
      if (rc2 != 0) return -rc2;
      data = view.data();
      len = view.size();
      break;
    }
    case Handle::Kind::Dir:
      return -EISDIR;
  }

  // Slice [offset, offset+size) with bounds clipping (RFC 0000 §3.3).
  if (offset < 0) return -EINVAL;
  const std::uint64_t off = static_cast<std::uint64_t>(offset);
  if (off >= len) return 0;
  const std::size_t copy = static_cast<std::size_t>(std::min<std::uint64_t>(size, len - off));
  std::memcpy(buf, data + off, copy);
  return static_cast<int>(copy);
}

int Gitmount::cb_release(const char*, struct fuse_file_info* fi) {
  delete reinterpret_cast<Handle*>(fi->fh);
  fi->fh = 0;
  return 0;
}

int Gitmount::cb_statfs(const char*, struct statvfs* st) {
  Gitmount* fs = self();
  std::lock_guard<std::mutex> lk(fs->mu_);
  const std::uint64_t bytes = fs->repo_->odb_disk_bytes();
  std::memset(st, 0, sizeof(*st));
  st->f_bsize = 4096;
  st->f_frsize = 4096;
  st->f_blocks = (bytes + 4095) / 4096;  // local ODB usage only (§3.3)
  st->f_bfree = 0;                       // read-only volume convention: df shows 100% used
  st->f_bavail = 0;
  st->f_files = 0;  // exact inode counting would need a full walk (§3.3)
  st->f_ffree = 0;
  return 0;
}

int Gitmount::cb_erofs() { return -EROFS; }

const fuse_operations* Gitmount::fuse_ops() {
  static const fuse_operations ops = [] {
    fuse_operations o{};
    o.getattr = cb_getattr;
    o.readlink = cb_readlink;
    o.opendir = cb_opendir;
    o.readdir = cb_readdir;
    o.releasedir = cb_releasedir;
    o.open = cb_open;
    o.read = cb_read;
    o.release = cb_release;
    o.statfs = cb_statfs;
    o.flush = [](const char*, struct fuse_file_info*) { return 0; };
    o.fsync = [](const char*, int, struct fuse_file_info*) { return 0; };
    // Write paths: EROFS everywhere (RFC 0000 §3.3). xattr handlers are
    // deliberately NOT registered: the kernel short-circuits to ENOTSUP.
    o.mknod = [](const char*, mode_t, dev_t) { return -EROFS; };
    o.mkdir = [](const char*, mode_t) { return -EROFS; };
    o.unlink = [](const char*) { return -EROFS; };
    o.rmdir = [](const char*) { return -EROFS; };
    o.symlink = [](const char*, const char*) { return -EROFS; };
    o.rename = [](const char*, const char*, unsigned int) { return -EROFS; };
    o.link = [](const char*, const char*) { return -EROFS; };
    o.chmod = [](const char*, mode_t, struct fuse_file_info*) { return -EROFS; };
    o.chown = [](const char*, uid_t, gid_t, struct fuse_file_info*) { return -EROFS; };
    o.truncate = [](const char*, off_t, struct fuse_file_info*) { return -EROFS; };
    o.create = [](const char*, mode_t, struct fuse_file_info*) { return -EROFS; };
    o.write = [](const char*, const char*, size_t, off_t, struct fuse_file_info*) {
      return -EROFS;
    };
    o.utimens = [](const char*, const struct timespec[2], struct fuse_file_info*) {
      return -EROFS;
    };
    return o;
  }();
  return &ops;
}

}  // namespace gitmount
