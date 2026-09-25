// gitmount — read-only git-to-FUSE filesystem (RFC 0000).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gitrepo.hpp"

#include <cstring>
#include <filesystem>

#include "errmap.hpp"
#include "log.hpp"

namespace fs = std::filesystem;

namespace gitmount {

// ---------------------------------------------------------------------------
// libgit2 process-wide state
// ---------------------------------------------------------------------------

void libgit2_global_init() { git_libgit2_init(); }

void libgit2_global_shutdown() { git_libgit2_shutdown(); }

void libgit2_configure_cache(std::uint64_t tree_cache_bytes) {
  // RFC 0000 §3.5: raise the tree per-type limit (default 4 KiB starves
  // serialized trees), raise COMMIT alongside (defensive, no cost), pin
  // BLOB to 0 so libgit2 never caches blob bytes that gitmount's own LRU
  // already accounts for, and set the total budget to the user's value.
  git_libgit2_opts(GIT_OPT_SET_CACHE_OBJECT_LIMIT, GIT_OBJECT_TREE, static_cast<size_t>(1) << 20);
  git_libgit2_opts(GIT_OPT_SET_CACHE_OBJECT_LIMIT, GIT_OBJECT_COMMIT, static_cast<size_t>(1) << 20);
  git_libgit2_opts(GIT_OPT_SET_CACHE_OBJECT_LIMIT, GIT_OBJECT_BLOB, static_cast<size_t>(0));
  git_libgit2_opts(GIT_OPT_SET_CACHE_MAX_SIZE, tree_cache_bytes);
}

// ---------------------------------------------------------------------------
// RAII helpers
// ---------------------------------------------------------------------------

namespace {

template <typename T, void (*Free)(T*)>
struct UniqueHandle {
  struct Deleter {
    void operator()(T* p) const {
      if (p) Free(p);
    }
  };
  using Ptr = std::unique_ptr<T, Deleter>;
};

using RepositoryPtr = UniqueHandle<git_repository, git_repository_free>::Ptr;
using ReferencePtr = UniqueHandle<git_reference, git_reference_free>::Ptr;
using ObjectPtr = UniqueHandle<git_object, git_object_free>::Ptr;
using TreePtr = UniqueHandle<git_tree, git_tree_free>::Ptr;
using CommitPtr = UniqueHandle<git_commit, git_commit_free>::Ptr;
using OdbObjectPtr = UniqueHandle<git_odb_object, git_odb_object_free>::Ptr;
using RevwalkPtr = UniqueHandle<git_revwalk, git_revwalk_free>::Ptr;
using IteratorPtr = UniqueHandle<git_reference_iterator, git_reference_iterator_free>::Ptr;

std::string last_git_error(const char* fallback) {
  const git_error* e = git_error_last();
  return e && e->message ? e->message : fallback;
}

}  // namespace

// ---------------------------------------------------------------------------
// .gitmodules parsing
// ---------------------------------------------------------------------------

namespace {

std::string strip(std::string s) {
  std::size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  std::size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

std::string unquote(const std::string& v) {
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
    std::string inner = v.substr(1, v.size() - 2);
    std::string out;
    for (std::size_t i = 0; i < inner.size(); ++i) {
      if (inner[i] == '\\' && i + 1 < inner.size()) {
        ++i;
        out += inner[i];
      } else {
        out += inner[i];
      }
    }
    return out;
  }
  return v;
}

}  // namespace

std::vector<std::pair<std::string, std::string>> parse_gitmodules(const std::string& content) {
  std::vector<std::pair<std::string, std::string>> out;
  std::string section;
  std::string cur_name, cur_path, cur_url;
  bool in_submodule = false;

  auto flush = [&]() {
    if (in_submodule && !cur_path.empty()) out.emplace_back(cur_path, cur_url);
    cur_name.clear();
    cur_path.clear();
    cur_url.clear();
    in_submodule = false;
  };

  std::size_t pos = 0;
  while (pos < content.size()) {
    std::size_t nl = content.find('\n', pos);
    std::string line = content.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = (nl == std::string::npos) ? content.size() : nl + 1;

    std::size_t hash = line.find_first_of(";#");
    if (hash != std::string::npos) line = line.substr(0, hash);
    line = strip(line);
    if (line.empty()) continue;

    if (line.front() == '[') {
      std::size_t close = line.find(']');
      if (close == std::string::npos) continue;
      std::string header = strip(line.substr(1, close - 1));
      flush();
      // Only [submodule "name"] sections carry path/url.
      if (header.rfind("submodule", 0) == 0) {
        std::size_t q1 = header.find('"');
        std::size_t q2 = header.rfind('"');
        if (q1 != std::string::npos && q2 > q1) cur_name = unquote(header.substr(q1, q2 - q1 + 1));
        in_submodule = true;
      }
      continue;
    }
    std::size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = strip(line.substr(0, eq));
    std::string value = unquote(strip(line.substr(eq + 1)));
    if (!in_submodule) continue;
    if (key == "path") cur_path = value;
    if (key == "url") cur_url = value;
  }
  flush();
  return out;
}

// ---------------------------------------------------------------------------
// GitRepo
// ---------------------------------------------------------------------------

std::string oid_to_hex(const git_oid& oid) {
  char buf[GIT_OID_MAX_HEXSIZE + 1];
  git_oid_tostr(buf, sizeof(buf), &oid);
  return buf;
}

std::unique_ptr<GitRepo> GitRepo::open(const std::string& path, std::string* err) {
  git_repository* repo = nullptr;
  int rc = git_repository_open(&repo, path.c_str());
  if (rc != 0) {
    if (err) *err = last_git_error("cannot open repository");
    return nullptr;
  }
  auto out = std::unique_ptr<GitRepo>(new GitRepo());
  out->repo_.reset(repo);
  out->gitdir_ = git_repository_path(repo);
  return out;
}

std::vector<std::string> GitRepo::all_refs() {
  std::vector<std::string> names;
  git_reference_iterator* it = nullptr;
  if (git_reference_iterator_new(&it, repo_.get()) != 0) return names;
  IteratorPtr guard(it);
  git_reference* ref = nullptr;
  while (git_reference_next(&ref, it) == 0) {
    ReferencePtr rguard(ref);
    names.push_back(git_reference_name(ref));
    ref = nullptr;
  }
  return names;
}

std::vector<std::string> GitRepo::refs_under(const std::string& prefix) {
  std::vector<std::string> out;
  const std::string p = prefix + "/";
  for (auto& name : all_refs())
    if (name.size() > p.size() && name.compare(0, p.size(), p) == 0) out.push_back(name);
  return out;
}

std::optional<RefInfo> GitRepo::lookup_ref(const std::string& full_name) {
  git_reference* ref = nullptr;
  int rc = git_reference_lookup(&ref, repo_.get(), full_name.c_str());
  if (rc != 0) return std::nullopt;
  ReferencePtr guard(ref);
  RefInfo info;
  info.symbolic = git_reference_type(ref) == GIT_REFERENCE_SYMBOLIC;
  if (info.symbolic) {
    git_reference* resolved = nullptr;
    if (git_reference_resolve(&resolved, ref) != 0) return std::nullopt;
    ReferencePtr rguard(resolved);
    info.target = *git_reference_target(resolved);
  } else {
    const git_oid* t = git_reference_target(ref);
    if (!t) return std::nullopt;
    info.target = *t;
  }
  return info;
}

std::string GitRepo::symbolic_target(const std::string& full_name) {
  git_reference* ref = nullptr;
  if (git_reference_lookup(&ref, repo_.get(), full_name.c_str()) != 0) return "";
  ReferencePtr guard(ref);
  if (git_reference_type(ref) != GIT_REFERENCE_SYMBOLIC) return "";
  const char* t = git_reference_symbolic_target(ref);
  return t ? t : "";
}

HeadInfo GitRepo::head() {
  HeadInfo info;
  git_reference* ref = nullptr;
  int rc = git_repository_head(&ref, repo_.get());
  if (rc == GIT_EUNBORNBRANCH || rc == GIT_ENOTFOUND) {
    info.state = HeadInfo::State::Unborn;
    return info;
  }
  if (rc != 0) {
    info.state = HeadInfo::State::Unborn;  // defensive; unreachable in practice
    return info;
  }
  ReferencePtr guard(ref);
  if (git_repository_head_detached(repo_.get())) {
    info.state = HeadInfo::State::Detached;
    info.oid = *git_reference_target(ref);
  } else {
    info.state = HeadInfo::State::Symbolic;
    // git_repository_head returns the resolved branch ref; the symbolic
    // target name comes from HEAD itself.
    info.symbolic_target = symbolic_target("HEAD");
  }
  return info;
}

int GitRepo::peel_to_commit(const git_oid& target, git_oid* commit_out) {
  git_object* obj = nullptr;
  int rc = git_object_lookup(&obj, repo_.get(), &target, GIT_OBJECT_ANY);
  if (rc != 0) return git_to_errno(rc);
  ObjectPtr guard(obj);
  git_object* peeled = nullptr;
  rc = git_object_peel(&peeled, obj, GIT_OBJECT_COMMIT);
  if (rc != 0) return git_to_errno(rc);  // EINVAL for tag->blob/tree chains
  ObjectPtr pguard(peeled);
  *commit_out = *git_object_id(peeled);
  return 0;
}

int GitRepo::commit_committer_time(const git_oid& commit, std::int64_t* time_out) {
  git_commit* c = nullptr;
  int rc = git_commit_lookup(&c, repo_.get(), &commit);
  if (rc != 0) return git_to_errno(rc);
  CommitPtr guard(c);
  *time_out = static_cast<std::int64_t>(git_commit_committer(c)->when.time);
  return 0;
}

int GitRepo::commit_root_tree(const git_oid& commit, git_oid* tree_out) {
  git_commit* c = nullptr;
  int rc = git_commit_lookup(&c, repo_.get(), &commit);
  if (rc != 0) return git_to_errno(rc);
  CommitPtr guard(c);
  *tree_out = *git_commit_tree_id(c);
  return 0;
}

bool GitRepo::is_commit_object(const git_oid& oid) {
  git_odb* odb = nullptr;
  if (git_repository_odb(&odb, repo_.get()) != 0) return false;
  size_t len = 0;
  git_object_t type = GIT_OBJECT_ANY;
  int rc = git_odb_read_header(&len, &type, odb, &oid);
  git_odb_free(odb);
  return rc == 0 && type == GIT_OBJECT_COMMIT;
}

std::optional<std::vector<TreeEntry>> GitRepo::tree_entries(const git_oid& tree) {
  git_tree* t = nullptr;
  if (git_tree_lookup(&t, repo_.get(), &tree) != 0) return std::nullopt;
  TreePtr guard(t);
  std::vector<TreeEntry> out;
  const size_t n = git_tree_entrycount(t);
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    const git_tree_entry* e = git_tree_entry_byindex(t, i);
    if (!e) continue;
    TreeEntry te;
    te.name = git_tree_entry_name(e);
    te.mode = git_tree_entry_filemode(e);
    te.oid = *git_tree_entry_id(e);
    out.push_back(std::move(te));
  }
  return out;
}

std::optional<TreeEntry> GitRepo::tree_entry(const git_oid& tree, const std::string& name) {
  git_tree* t = nullptr;
  if (git_tree_lookup(&t, repo_.get(), &tree) != 0) return std::nullopt;
  TreePtr guard(t);
  const git_tree_entry* e = git_tree_entry_byname(t, name.c_str());
  if (!e) return std::nullopt;
  TreeEntry te;
  te.name = git_tree_entry_name(e);
  te.mode = git_tree_entry_filemode(e);
  te.oid = *git_tree_entry_id(e);
  return te;
}

int GitRepo::blob_size(const git_oid& oid, std::uint64_t* size_out) {
  git_odb* odb = nullptr;
  if (git_repository_odb(&odb, repo_.get()) != 0) return EIO;
  git_odb* owned = odb;  // free below on all paths
  size_t len = 0;
  git_object_t type = GIT_OBJECT_ANY;
  int rc = git_odb_read_header(&len, &type, owned, &oid);
  git_odb_free(owned);
  if (rc != 0) return git_to_errno(rc);
  *size_out = len;
  return 0;
}

int GitRepo::read_blob(const git_oid& oid, std::string* out) {
  git_odb* odb = nullptr;
  if (git_repository_odb(&odb, repo_.get()) != 0) return EIO;
  git_odb_object* obj = nullptr;
  int rc = git_odb_read(&obj, odb, &oid);
  git_odb_free(odb);
  if (rc != 0) return git_to_errno(rc);
  OdbObjectPtr guard(obj);
  const char* data = static_cast<const char*>(git_odb_object_data(obj));
  const size_t size = git_odb_object_size(obj);
  out->assign(data, size);
  return 0;
}

int GitRepo::list_commits(const std::vector<git_oid>& sorted_push_oids,
                          const std::function<void()>& unlock, const std::function<void()>& lock,
                          std::string* out) {
  git_revwalk* walk = nullptr;
  int rc = git_revwalk_new(&walk, repo_.get());
  if (rc != 0) return git_to_errno(rc);
  RevwalkPtr guard(walk);

  // Pinned determinism (RFC 0000 §3.5): topological order with a fixed push
  // order (ref names sorted by the caller; HEAD last).
  git_revwalk_sorting(walk, GIT_SORT_TOPOLOGICAL);
  for (const git_oid& oid : sorted_push_oids) {
    rc = git_revwalk_push(walk, &oid);
    if (rc == GIT_ENOTFOUND) {
      // Object vanished (external gc/prune race); the caller retries the
      // whole generation — treat as generation failure (EIO semantics).
      return EIO;
    }
    if (rc != 0) return git_to_errno(rc);
  }

  // Chunked revwalk (RFC 0000 §3.4): process 4096 git_revwalk_next calls,
  // then drop and re-acquire the single mutex so interactive requests can
  // interleave. The walker itself is owned by this thread.
  constexpr std::size_t kChunk = 4096;
  std::size_t in_chunk = 0;
  git_oid oid;
  char hex[GIT_OID_MAX_HEXSIZE + 1];
  for (;;) {
    rc = git_revwalk_next(&oid, walk);
    if (rc == GIT_ITEROVER) break;
    if (rc != 0) return EIO;  // object vanished mid-walk (gc/prune race)
    git_oid_tostr(hex, sizeof(hex), &oid);
    out->append(hex);
    out->push_back('\n');
    if (++in_chunk == kChunk) {
      in_chunk = 0;
      unlock();
      lock();
    }
  }
  return 0;
}

std::uint64_t GitRepo::odb_disk_bytes() {
  std::uint64_t total = 0;
  std::error_code ec;
  fs::path objects = fs::path(gitdir_) / "objects";

  // Packfiles (*.pack only; .idx and friends are not packfile bytes).
  fs::path pack = objects / "pack";
  for (fs::directory_iterator it(pack, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    if (it->path().extension() == ".pack") total += it->file_size(ec);
  }
  ec.clear();

  // Loose objects: two-hex-char subdirectories.
  for (fs::directory_iterator it(objects, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code dec;
    if (!it->is_directory(dec)) continue;
    const std::string name = it->path().filename().string();
    if (name.size() != 2) continue;
    bool hex = true;
    for (char c : name)
      if (!std::isxdigit(static_cast<unsigned char>(c))) hex = false;
    if (!hex) continue;
    for (fs::directory_iterator sub(it->path(), dec), subend; !dec && sub != subend;
         sub.increment(dec)) {
      if (!sub->is_regular_file(dec)) continue;
      total += sub->file_size(dec);
    }
  }
  return total;
}

bool GitRepo::alternates_active() {
  std::error_code ec;
  return fs::exists(fs::path(gitdir_) / "objects" / "info" / "alternates", ec);
}

}  // namespace gitmount
