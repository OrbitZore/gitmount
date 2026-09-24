# RFC 0000 — gitfs：将 git 仓库挂载为只读 FUSE 文件系统

- RFC 编号: 0000
- 标题: gitfs — read-only git-to-FUSE 文件系统
- 状态: Accepted（2026-09-24 评审通过，决议见 §7；2026-09-24 补充决议见 §7.1、§7.2）
- 日期: 2026-09-24
- 目标版本: 0.1.0

## 0. 摘要

实现一个 C++17 编写的命令行程序 `gitfs`，把一个本地 bare 或普通 git 仓库挂载为
**只读** FUSE 文件系统。挂载点根目录暴露下列入口：

```
/mnt/gitfs/
├── branch/      # 本地分支
├── tag/         # tag（annotated 与 lightweight）
├── commit/      # 任意 commit（按完整 oid 访问：sha1=40、sha256=64 位，不可枚举）
├── remote/      # 远端跟踪分支 <remote>/<branch>
├── HEAD/        # 当前 HEAD 指向的快照
├── commits      # 只读文件：全部可达 commit（自 refs 与 HEAD）的
│                #   完整 oid 清单（grep 自取）
└── .gitfs.json  # 挂载元信息
```

每个 ref（分支/tag/远端跟踪 ref/HEAD）或 commit 解析为一个 commit 对象，其
root tree 以真实目录树形式呈现，用户无需 `checkout` 即可用普通工具（`ls`、`cat`、`grep`、diff 工具）
浏览任意历史版本的文件内容。

技术选型：**libgit2**（对象访问）+ **libfuse3**（文件系统）+ CMake 构建。

## 1. 动机

- 在多个版本之间快速对比、检索，而不必反复 `git worktree add` / `git archive`。
- 只读语义天然安全：任何工具对该挂载的写入都会得到 `EROFS`，不可能污染仓库。
- 现有方案（如 presslabs/gitfs）主要面向"当前分支 + 历史"，或绑定特定云存储；
  我们需要一个轻量、语义清晰、C++ 实现、符合开源工程规范的基础组件。

## 2. 目标 / 非目标

### 目标（v0.1）

1. 只读文件系统；所有写操作返回 `EROFS`，挂载参数强制 `ro`。
2. 根目录暴露 `branch/`、`tag/`、`commit/`、`remote/`、`HEAD/` 五个子目录
   与 `commits`、`.gitfs.json` 两个合成文件。
3. 完整呈现 git tree：目录（`040000`）、普通文件（`100644`）、可执行文件
   （`100755`）、符号链接（`120000`）；子模块（`160000`）以空目录 + 说明文件占位。
4. 对普通文件支持 `read`/`mmap`（不可变 blob，天然适合内核页缓存）。
5. 单机 Linux（fuse3），C++17，依赖仅 libgit2（≥1.4，提供 oid 类型查询）
   + libfuse3（≥3.10），CI 矩阵验证。
6. 完整的开源工程配套：CMake、单测+集成测试、CI、文档、规范提交。

### 非目标（v0.1，未来另立 RFC）

- 任何写路径（不支持写入、暂存、提交）。
- 网络/远端操作（fetch/push/clone），不访问网络。
- 稀疏检出、部分克隆（partial clone）支持。
- macOS（osxfuse/macFUSE）与 BSD 移植（接口层预留抽象）。
- 工作区（index）视图、stash 视图。

## 3. 详细设计

### 3.1 路径与对象映射

路径解析按 **路径分量（component）逐级映射到 git 对象**，绝不拼接字符串后
`realpath`，因此不存在路径穿越问题：

```
/                        → 根（固定入口 + 合成文件，见 3.6）
/branch                  → git_branch_iterator 枚举本地分支
/branch/<name>           → 该分支 tip commit 的 root tree
/branch/<name>/a/b.txt   → tree 递归下行
/tag/<name>              → tag 对象 peel 至 commit 后的 root tree；
                           tag 直接指向 tree/blob（git 允许）→ ENOENT
                           并记警告日志，v0.1 不呈现非 commit 目标
/remote                  → 枚举 refs/remotes/ 下实际存在的首层命名空间
                           （refdb 直读；不用 git_remote_list，后者仅列配置项，
                           可能与残留跟踪 ref 不一致）
/remote/<remote>/<b>     → refs/remotes/<remote>/<b> 的 root tree；remote
                           名不含 `/`，路径在首个分量切分，其余整体为
                           分支路径（可含子路径），无歧义；
                           refs/remotes/<remote>/HEAD 符号引用**隐藏**
                           （枚举与查找均跳过，访问 → ENOENT）：peel 后
                           仅是默认分支树的重复，符号语义也无法在目录树中表达
/HEAD                    → 当前 HEAD commit 的 root tree（unborn → ENOENT）
/commit/<full-oid>       → 该 commit 的 root tree；oid 长度随仓库对象格式
                           （sha1=40、sha256=64），格式非法 → ENOENT；
                           oid 须指向 commit 对象本身，不做 peel——
                           annotated tag/tree/blob 的 oid → ENOENT
/commits                 → 只读文件：全部可达 commit（可达自 refs
                           与 HEAD）的完整 oid，每行一条，
                           拓扑序（保证确定性）；不支持短前缀解析，用户
                           自行 `grep ^<前缀> commits` 检索
```

- `/commit` 的 readdir **返回空列表**（不可枚举）：巨型仓库（linux 内核
  ~110 万 commit）在 readdir 的 offset 续读语义下要么缓存 ~100MB 条目、
  要么 O(n²) 重走 revwalk，且首次全量遍历耗时数秒会挂起列目录进程；
  oid 检索需求改由 `commits` 清单文件承担（首次 `open` 时生成、缓存，
  整块缓冲、按 offset 切片读，见 3.5）。

- ref 名规则（git check-ref-format）禁用空格、控制字符及 `~^:?*[\` 等，
  天然是合法文件名；可含 Unicode（如中文），直接作为文件名。
- **tree entry 名不受 check-ref-format 约束**，可为任意字节（空格、控制
  字符、非 UTF-8 序列）：git 对象格式保证 entry 名不含 `/`（含 `/` 会被
  拆成嵌套 tree）与 NUL（tree 条目的格式分隔符），因此**按原始字节透传**
  给 VFS 即安全，不做任何改写/转义；超长仍按 NAME_MAX → `ENAMETOOLONG`
  （见 3.3）。非 UTF-8 名字在用户侧显示为乱码属预期，与 `git checkout`
  行为一致。**唯一例外**是 `.`/`..`：手工构造（绕过 fsck）的对象可产生
  这类 entry，与 VFS 保留名冲突，readdir/getattr **跳过**（getattr →
  `ENOENT`）并记警告日志，不透传给 VFS（fuse3 的 `.`/`..` 由内核生成，
  readdir 亦不得返回）。
- **`refs/replace` 不生效（透传原始对象）**：gitfs 不遵循 replace refs，
  语义等同 `git --no-replace-objects`——`/commit/<oid>` 的内容寻址承诺
  （oid ↔ 对象一一对应）优先于替换机制；代价是在设置过 replace refs 的
  仓库中输出与默认配置的 `git cat-file`/`git ls-tree` 不同（§4 测试
  oracle 一律带 `--no-replace-objects`）。libgit2（截至 1.9）本就不实现
  replace refs，透传即其默认行为；若未来版本引入遵循开关，构建时显式
  关闭并加单测防回归。
- **含 `/` 的 ref 名（如分支 `feature/foo`、tag `v1.0/rc`、远端跟踪
  `origin/feature/x`）按嵌套目录渲染**：
  readdir 按首分量分组；查找时逐级累积分量查询 refdb。git refdb 的 D/F
  规则保证一个 ref 不会是另一个的前缀，因此至多存在一个完整 ref 名前缀
  匹配，剩余分量即 tree 路径，无歧义；分组前缀节点与完整 ref 节点均为
  目录，getattr 语义一致。
- `/branch`、`/tag`、`/remote` 的 readdir 实时反映仓库外部更新（`/commit`
  恒为空，见上），新 commit、新 tag 无需重新挂载即可见；已解析的旧对象只要
  仍存在于 ODB 中就继续可访问（配合 3.5 的缓存策略）。
- **空仓库（无任何 refs 且 unborn HEAD）**：挂载照常成功——`/branch`、
  `/tag`、`/remote` 为空目录，`/HEAD` 访问 → `ENOENT`，`commits` 为空
  清单（`st_size=0`，open 生成空缓冲），`.gitfs.json` 的 `head` 为
  `null`（见 3.6）。
- **挂载期间的外部 `git gc`/`git prune`**：gitfs 不加锁、不阻止任何外部
  git 操作。若 gc 重写 pack 或 prune 删除了后续请求仍需要的对象，该请求
  瞬时返回 `ENOENT`（对象消失）或 `EIO`（pack 中途失效），gc 结束后即
  恢复；gitfs 不承诺挂载期内对象集不变。运维建议：挂载期间禁用自动
  gc（`git config gc.auto 0`）或接受上述瞬态错误（README 与
  filesystem-semantics.md 中声明，见 §4）。

### 3.2 权限与元数据映射

| git entry mode | st_mode | 说明 |
|---|---|---|
| `040000` | `S_IFDIR \| 0755` | 目录 |
| `100644` | `S_IFREG \| 0644` | 普通文件 |
| `100755` | `S_IFREG \| 0755` | 可执行 |
| `120000` | `S_IFLNK \| 0777` | 符号链接，`readlink` 返回 blob 内容 |
| `160000` | `S_IFDIR \| 0755`（空目录）+ 说明文件 | 子模块：渲染为空目录，旁边放 `<name>.gitfs-submodule` 文本文件（`<name>` 即该子模块目录名，如 `deps/libfoo` → `deps/libfoo.gitfs-submodule`，与 man 页措辞一致；内容含 url 与 commit oid）；若树中已存在与该合成文件名相同的真实条目，真实条目优先、省略合成文件（记警告日志） |

- `st_size`：blob 的原始字节数（libgit2 直读，不做过滤）。
- `st_mtime`/`st_ctime`：所属 commit 的 committer time；同一快照内全部一致，
  便于 rsync 类工具判断。
- `st_atime`：与 `st_mtime` 同值（committer time），不随读取更新——只读
  快照语义，避免逐次 stat 漂移干扰 rsync 类工具；主流发行版 `relatime`
  挂载下内核本就不强制刷新 atime，用户无感知。
- `st_nlink`：目录为 2，文件为 1（简化，不做精确统计）。
- `st_uid`/`st_gid`：挂载进程的 uid/gid（fuse 默认行为）。
- 合成文件的 `st_mtime`：`commits` 为其生成时刻；`.gitfs-submodule` 为所属
  commit 的 committer time（内容确定性派生自树，避免逐次 stat 漂移）；
  `.gitfs.json` 为挂载时刻（快照语义，见 3.6）。
- `.gitfs-submodule` 说明文件：mode `0644`，内容为两行 `key=value` 文本——
  `url=<submodule url>` 与 `commit=<完整 oid>`（各以 LF 结尾）；url 取自该
  commit 树根 `.gitmodules` 中对应 path 的条目，缺失或无对应条目时 `url=`
  置空并记警告日志。

### 3.3 FUSE 操作实现

| 操作 | 行为 |
|---|---|
| `getattr` | 路径 → 对象（3.1），失败 `ENOENT`；`commits` 恒为纯缓存读（未生成时 `st_size=0`），**不**触发 revwalk 或指纹重算（见 3.5） |
| `readdir` | 根：固定列表；`branch/tag/remote`：枚举 ref（含 `/` 的名字按目录分组）；`commit`：**恒为空**；tree：枚举 entries（含 `.gitfs-submodule` 合成项）。注册 `opendir/releasedir`：枚举列表快照挂于 `fi->fh`，保证单目录流内 offset 续读稳定（fuse3 要求），跨目录流实时反映 ref 变化 |
| `open`/`release` | 仅校验 `O_RDONLY` 系标志（写标志 → `EROFS`，与内核对 ro 挂载的判定一致）；`commits` 首次 `open` 触发生成（见 3.5），且把当前缓冲版本**钉住于 `fi->fh`**——同一次 open 的所有 read 分片读自同一快照，refs 中途变化不影响（与 readdir 的目录流快照同构），`release` 时解除钉住；`.gitfs.json` 挂载期内不可变，无需钉住 |
| `read` | 定位 blob（经 LRU 缓存），拷贝 `[offset, offset+size)` 越界截断；合成文件（`commits`、`.gitfs.json`、`.gitfs-submodule`）为整块只读缓冲，`commits` 读 open 时钉住的版本 |
| `readlink` | symlink blob 内容；内容含嵌入 NUL 时**截断至首个 NUL**（内核 symlink 目标不可含 NUL；与 `git checkout` 的事实行为一致，显式同语义而非 `EIO`）；空 blob → 返回长度 0 的空目标；超过 PATH_MAX → `ENAMETOOLONG` |
| `statfs` | 汇报本地 ODB 占用为 `f_blocks`（全部 packfile 字节 + loose 对象字节；alternates 指向的外部存储不计入，启用 alternates 时 verbose 日志提示），块大小 4KiB；`f_bfree = f_bavail = 0`——只读卷惯例是 0 空闲，`df` 显示 100% 已用，向用户明确传达"无任何可写空间"（若报全量可用，`df` 会显示 0% 已用，易误导）；`f_files = f_ffree = 0`（精确 inode 计数需全量遍历，v0.1 不承诺，内核与 `df` 均容忍 0） |
| 其余（`mknod/mkdir/write/…`） | 返回 `EROFS` 或不注册（fuse3 只读挂载兜底） |

错误映射：libgit2 错误码 → `ENOENT`（对象/分支不存在；oid 格式非法也统一
`ENOENT`，不向调用方暴露内部规则）、`EIO`（ODB 损坏）、`ENAMETOOLONG`
（文件名超过 NAME_MAX）等，集中在一个 `git_to_errno()` 翻译函数。readlink
的 NUL 截断/空目标（见上表）是**非错误**的降级语义，同样在该翻译层
旁注明，避免实现时误报 `EIO`。

### 3.4 进程模型与并发

- v0.1 以 **fuse3 多线程模式** 运行，但所有 libgit2 调用集中在 `GitRepo`
  包装类内，受单个 `std::mutex` 串行化（libgit2 全局线程需 `git_libgit2_init`，
  且 1.x 对同 repository 的并发读有保证，仍保守串行化，性能瓶颈在 I/O 而非锁）。
- **长任务例外——`/commits` 生成的 revwalk 分 chunk 执行，不整段持锁**：
  若整段持有上述 mutex，大仓库（百万 commit、数秒）首次 `open /commits`
  期间会阻塞全挂载的 getattr/readdir/read，与 3.1 避免"秒级 revwalk
  挂起"的初衷相悖。实现为：每批 `git_revwalk_next` 一定数量（如 4096）
  后释放 mutex 让其他请求插队，随后重取继续，walker 本身仍归生成线程
  独占（revwalk 句柄非线程安全）；释放窗口内其他线程只做同 repository
  的并发只读操作，libgit2 1.x 已保证安全。单 chunk 持锁时间为毫秒级，
  交互请求延迟不可感知；清单总生成耗时数量级不变，该最坏停顿语义在
  README 中声明（首次 `open /commits` 的返回时间仍与 revwalk 总耗时
  同阶，读方需预期等待）。**并发首次 open 以 single-flight 串行化**：
  生成由一次性互斥（once/双检锁）保护——首个触发线程在锁内重查
  "已生成且指纹一致"后才启动 revwalk，其余并发 open 阻塞等待同一次
  生成完成后直接复用缓冲；不做两个 revwalk 同时跑的浪费，也不存在
  缓冲交换竞态（读方拿到的 `fi->fh` 引用自始有效）。
- RAII 封装所有 libgit2 句柄（`git_repository`、`git_tree`、`git_blob`…），
  自定义 deleter 的 `std::unique_ptr` 别名，异常安全，热路径无异常。
- `SIGINT`/`SIGTERM` → `fuse_session_exit` 优雅卸载；同时支持 `umount` 外部卸载。

### 3.5 缓存与性能

- **blob LRU 缓存**：键 = blob oid，值 = 不可变字节串；容量按字节计，
  默认 64 MiB，`--cache-size` 可调。命中则 `read` 为纯内存拷贝。
- **tree/commit 依赖 libgit2 内置对象缓存，init 时显式调参**：默认 per-type
  上限仅 4KiB（源码 `cache.c` 的 `git_cache__max_object_size[]`），序列化
  超线的大目录 tree（~100+ entry 即超）不进缓存；而高层 fuse API 每个
  syscall 自 root tree 重解析 O(depth) 次、attr/entry_timeout 又显式置 0
  （见下），tree 命中率就是 `ls -R`/`find`/`du`/rsync 类元数据负载性能的
  全部。故 init 时以 `GIT_OPT_SET_CACHE_OBJECT_LIMIT(GIT_OBJECT_TREE, 1MiB)`
  抬线，COMMIT 同抬（commit 天然 <4KiB，仅防御病态巨型 merge commit，
  无代价）；总预算 `GIT_OPT_SET_CACHE_MAX_SIZE` 维持默认 256MiB（1.9.7
  实测），与 `--cache-size` **各自独立记账、互不挤占**。
- **blob 保证不进 libgit2 缓存（防双层缓存）**：per-type 上限默认即 0
  （从不缓存；1.9.7 实测 lookup 后缓存计数恒 0，抬限后立即计入、再读
  命中），但这是默认值而非契约——任何一处
  `GIT_OPT_SET_CACHE_OBJECT_LIMIT(GIT_OBJECT_BLOB, n>0)` 都会让同一份
  字节在 libgit2 缓存（`git_blob` 包装）与自家 LRU（raw bytes）各存
  一份、双份记账。故 init 显式写死
  `GIT_OPT_SET_CACHE_OBJECT_LIMIT(GIT_OBJECT_BLOB, 0)`，令“恰好不
  重复”成为“保证不重复”。pack 读入走 packfile mmap → 内核页缓存，
  进程间共享、可回收，属正常分层而非堆内重复，与 LRU 互补不冲突；
  `GIT_OPT_ENABLE_CACHING` 保持默认开启（libgit2 公开 API 即此，不存在
  按 odb 实例设置缓存的接口）。
- `/commits` 清单：**首次 `open` 时** revwalk 全量生成（push 全部 refs
  **加 HEAD**——detached HEAD 独有的 commit 也纳入清单，与 §0 "全部可达"
  的宣称一致，即可达集 = refs ∪ HEAD；`refs/remotes/<remote>/HEAD`
  符号引用跳过——其目标分支本身已在枚举集合内，可达集不变），整块缓存；
  以 refs 指向集合**加 HEAD 指向**的指纹为失效键，任一变化才重建。
  revwalk 按 3.4 的分 chunk 锁策略执行，生成期间不长期阻塞其他请求。**生成与指纹比对只发生在 `open`**：生成前 `getattr` 报告
  `st_size = 0`，故 `ls -l`/`stat`/文件管理器枚举不触发秒级 revwalk，与
  3.1 将 `/commit` readdir 置空的同一理由保持一致；生成后 `st_size` 为
  真实值。单次 open 经 `fi->fh` 钉住缓冲版本直至 release（见 3.3），跨
  open 语义为"下一次 open 起可见新版本"；并发首次 open 的 single-flight
  串行化见 3.4。挂载后后台线程预生成留作后续
  可选优化（如 `--prewarm`），v0.1 不做。110 万 commit ≈ 45MB 文本
  （sha1 口径：每行 41B 含 LF；sha256 仓库为 65B/行 ≈ 74MB），内存与
  grep 均可接受。
- FUSE 侧默认 **不开启 kernel_cache**：显式置 `attr_timeout=0、
  entry_timeout=0`（注意 fuse3 默认值均为 1s，不显式置 0 则有 1 秒陈旧窗口）：
  ref 是可变的（分支可能被删），正确性优先；提供 `-o kernel_cache` 透传给
  高级用户在"仓库挂载期间不变化"场景下自行开启。blob 不可变，内核页缓存
  对 `mmap` 的收益不受影响。
- 预期性能目标与 CI 门禁：以**比值阈值**为主——`grep -r` 全树吞吐不低于
  `git archive | tar -x` 的 50%；冷缓存 `cat` 单文件耗时不超过同机
  `git cat-file blob` 基线的 1.5 倍。共享 runner 墙钟抖动大，绝对值断言
  （如 <5ms）仅在自管 runner 上作参考检查并带 ±30% 容差；比值门禁在
  共享 runner 上稳定执行，防性能回归。

### 3.6 根目录元信息

根目录额外暴露一个只读文件 `.gitfs.json`（名字以 `.` 开头，避免与入口目录
语义混淆）：

```json
{
  "format": 1,
  "repository": "/abs/path/to/repo.git",
  "head": "refs/heads/main",      # detached 时为完整 oid；unborn 时为 null
  "mounted_at": "2026-09-24T02:29:00Z",
  "cache_bytes": 67108864
}
```

便于脚本探测与调试；与仓库内容无命名冲突（根目录不呈现仓库树，合成文件
均为固定名）。

**失效语义（显式声明）**：`.gitfs.json` 是**挂载时刻的快照**，挂载期间
不可变——`repository`、`mounted_at`、`cache_bytes` 本就不随时间变化；
`head` 字段在挂载后分支切换/HEAD 移动时**不**跟随更新（需探测实时 HEAD
请进入 `HEAD/` 目录或直接 `git rev-parse`）。它不参与 refs 指纹失效
机制，重新挂载即刷新。

### 3.7 CLI 与退出码

```
用法: gitfs [选项] <repository> <mountpoint>

选项:
  -o OPT       透传 fuse 选项（如 kernel_cache；allow_other 需
               /etc/fuse.conf 启用 user_allow_other）；基线键
               （ro/nosuid/nodev/default_permissions）及其反向键
               （rw/suid/dev）出现即报错退出 1，防止安全基线被 CLI
               稀释或被反向键顶掉
  --cache-size <MiB>  blob LRU 缓存上限（默认 64）；值为正整数（十进制
                      MiB）——0、负数、非数字或溢出 size_t → 参数错误
                      （退出码 1）；不设人为上限，受可用内存约束
  --foreground / -f  前台运行（默认守护进程化）
  --verbose / -v     输出路径解析与缓存命中日志
  --version / --help
```

- 挂载选项硬编码基线：`ro,fsname=gitfs,default_permissions,subtype=gitfs,
  nosuid,nodev`。
- 退出码：0 正常卸载；1 参数错误；2 仓库不可读/不是 git 仓库；3 挂载失败。

### 3.8 安全注意事项

- `default_permissions` 让内核执行常规属主/权限检查，不依赖我们的实现。
- `ro + nosuid + nodev`：内容寻址的只读数据面，杜绝 setuid 与设备节点。
- symlink blob 内容由仓库控制，可能指向绝对路径或快照外目标，语义与
  `git checkout`/`tar -x` 一致，不做改写；用户不应以根/特权身份浏览不受信
  仓库的挂载点。`..` 逃逸由 VFS 挡在挂载点内。
- 仓库路径经 `realpath` 规范化为绝对路径（含符号链接解析）后打开，
  避免相对路径与 TOCTOU 问题。

## 4. 工程结构（开源最佳实践）

```
gitfs/
├── CMakeLists.txt            # >= 3.16，C++17，-Wall -Wextra -Wpedantic -Werror(CI)
├── LICENSE                   # GPL-3.0-or-later（SPDX 标注同左）
├── README.md                 # 快速开始、语义说明（含 /commits 首次 open
│                             # 的停顿语义，见 3.4；挂载期间禁 gc 的运维
│                             # 提示，见 3.1）、FAQ
├── CONTRIBUTING.md           # 分支/提交规范、如何跑测试
├── CODE_OF_CONDUCT.md
├── SECURITY.md               # 报告漏洞渠道
├── CHANGELOG.md              # Keep a Changelog 格式
├── .clang-format             # Google 风格（决议 Q6）
├── .gitignore / .gitattributes
├── .github/workflows/ci.yml  # lint + build + test 矩阵（gcc/clang × ubuntu）
├── rfc/                      # 本目录：设计文档先行
├── src/
│   ├── main.cpp              # CLI 解析、fuse 启动
│   ├── gitfs.hpp/.cpp        # fuse_ops 实现（路径解析、VFS 语义）
│   ├── gitrepo.hpp/.cpp      # libgit2 RAII 封装（ref 解析、tree 下行）
│   ├── object_cache.hpp      # blob LRU（单测覆盖）
│   ├── path_map.hpp/.cpp     # "/branch/x/y" → 解析状态机（纯函数，重点单测）
│   ├── errmap.cpp            # git_to_errno
│   └── log.hpp
├── tests/
│   ├── unit/                 # Catch2 v3（FetchContent）；path_map、cache、errmap
│   ├── integration/          # 真实挂载：fixture 仓库 + 断言 readdir/read/readlink
│   └── fixtures/make_repo.sh # 生成含子模块、symlink（含嵌入 NUL 的
│                              # symlink blob）、中文文件名、非 UTF-8 文件名
│                              # （原始字节透传）、嵌套分支名（feature/x）、
│                              # annotated 与 blob tag、可执行文件的测试仓库；
│                              # 并用 mktree/hash-object -w 手工构造含 '.'
│                              # 与 '..' entry 的非法 tree（绕过 fsck），
│                              # 供 readdir 跳过断言使用；另含 detached
│                              # HEAD 独有 commit（供 /commits 清单断言）
││                              # 与 refs/remotes/origin/HEAD（供隐藏断言）、
│                              # refs/replace/<oid>（供 replace 不生效断言）
└── docs/
    ├── filesystem-semantics.md  # 对用户承诺的语义（本文 3.x 的稳定化版本；
    │                          # 必含"gc/prune 并发"与"空仓库"两节，见 3.1）
    └── gitfs.1                  # man 手册（roff；CMake install 到 $(mandir)）
```

- **提交规范**：Conventional Commits（`feat:`/`fix:`/`docs:`…），CI 校验。
- **版本**：SemVer；打 tag 出 release，CI 产出各发行版二进制 + SBOM。
- **测试策略**：
  - 单测：Catch2 v3；覆盖 path_map 状态机（含畸形路径、Unicode、超长 oid）、
    LRU 逐出、错误映射表；
  - 集成：脚本生成 fixture 仓库 → 挂载到 tmpdir → 断言内容与
    `git --no-replace-objects ls-tree`/`git cat-file` 结果一致（oracle
    与 gitfs 同为"replace 不生效"语义，见 3.1；fixture 含 replace ref
    的反例断言）；含"挂载后新增 tag 立即可见"的一致性用例；
    边缘用例：`'.'`/`'..'` entry 在 readdir/getattr 被跳过且记警告、
    `refs/remotes/origin/HEAD` 在 `/remote` 不可见（访问 → `ENOENT`）、
    非 UTF-8 文件名按原始字节读回、含嵌入 NUL 的 symlink 截断至首个
    NUL、detached HEAD 独有 commit 出现在 `commits` 清单中；
  - CI 上 `/dev/fuse` 不可用时集成测试自动 skip（标记），在自管 runner 跑全量。
- **可观测性**：`-v/--verbose` 输出解析日志；错误信息含 oid 与 errno 上下文。

## 5. 备选方案

| 方案 | 结论 |
|---|---|
| 调用 `git` 子进程获取对象 | 否决：每次 fork+协议解析开销大、错误处理脆弱 |
| fuse2 | 否决：已进维护期，fuse3 是当前主线（本机 3.18 可用） |
| 自研 packfile 解析 | v0.1 否决：重复造轮子；保留为远期性能优化项 |
| 复用 presslabs/gitfs（Python） | 否决：语义不同（其为读写+云后端），且非本项目语言目标 |

## 6. 里程碑

1. **M1**：`/branch/<name>` 只读浏览 + 单测 —— 端到端可演示。
2. **M2**：`/tag`、`/commit`、`/remote`、`/HEAD`、`commits` 清单、错误映射、
   statfs、`.gitfs.json`。
3. **M3**：blob LRU、性能基准、集成测试矩阵、CI 完整化。
4. **M4**：文档定稿、0.1.0 发布（首个 SemVer tag）。

## 7. 评审决议（2026-09-24）

- **Q1/Q2 `/commit` 语义（已决）**：`/commit/<full-oid>` 仅接受完整 oid
  （长度随对象格式：sha1=40、sha256=64），
  **不支持短前缀解析**；`/commit` readdir 恒为空。oid 检索改由根目录只读
  文件 `commits` 承担（完整 oid 每行一条，用户自行 grep 前缀）。
- **Q3 子模块（已决）**：渲染为空目录 + 同名 `.gitfs-submodule` 说明文件。
- **Q4 ref 覆盖范围（已决）**：v0.1 即包含 `/remote/<remote>/<branch>` 与
  `/HEAD`。
- **Q5 许可证（已决）**：GPL-3.0-or-later。
- **Q6 代码风格（维护者定）**：clang-format Google 风格。
- **Q7 并发模型（维护者定）**：v0.1 单互斥串行化 libgit2；revwalk 长任务
  例外，分 chunk 让锁（见 3.4）；M3 基准不达标再引入按 oid 分片锁。

### 7.1 补充决议（2026-09-24，评审后 review 跟进）

- **Q8 `refs/replace`（已决）**：不遵循，透传原始对象（`git
  --no-replace-objects` 同语义）；libgit2 截至本决议（1.9）无该机制、
  无开关，未来版本引入则显式关闭并加回归单测（见 3.1、§4 测试 oracle）。
- **Q9 元数据补全（已决）**：`st_atime` ≡ `st_mtime`（不随读取更新）；
  `.gitfs-submodule` 定为 mode 0644、mtime = 所属 commit 的 committer
  time（修订原"生成时刻"表述，避免逐次 stat 漂移）、内容为 `url=`/
  `commit=` 两行（url 取自该 commit 树根 `.gitmodules`，缺失置空记警告）；
  unborn HEAD 时 `.gitfs.json` 的 `head` 为 `null`（见 3.2、3.6）。
- **Q10 statfs 口径与 CLI 黑名单（已决）**：`f_blocks` = 本地 ODB 占用
  （pack + loose，不含 alternates）；`-o` 拒绝名单补入反向键
  `rw/suid/dev`（见 3.3、3.7）。
- **Q11 libgit2 缓存调参与防双层缓存（已决）**：tree/commit 依赖
  libgit2 内置缓存，但显式抬 per-type 上限（tree 1MiB——默认 4KiB 会
  漏掉大目录 tree，拖垮元数据密集负载）；blob per-type **写死 0**，
  保证解压后 blob 仅存于自家 LRU、不双层缓存双记账；总预算维持默认
  256MiB，与 `--cache-size` 独立记账（见 3.5）。

### 7.2 勘误与语义补全（2026-09-24，review round 2 跟进）

- **Q12 文档一致性与运维语义（已决）**：(a) 时间线勘误——RFC 头部与 §7
  的日期由 2025-09-24 统一为 **2026-09-24**（与仓库提交、§7.1 及 §3.6
  示例时间戳一致），man 页头注释同步更新至 Q1-Q12；(b) §3.5 清单体
  积估算限定口径——45MB 仅按 sha1（41B/行），sha256 为 65B/行
  ≈ 74MB；(c) §3.2 子模块说明文件命名明确为 `<name>.gitfs-submodule`；
  (d) 显式声明挂载期间外部 `git gc`/`git prune` 语义——对象被删 →
  瞬时 `ENOENT`/`EIO`，建议挂载期间禁 gc 或接受瞬态错误（见 3.1）；
  (e) 显式声明空仓库行为——挂载成功、入口为空（见 3.1）；(f)
  `--cache-size` 校验规则定案：正整数，0/负数/非法 → 退出码 1，无
  人为上限（见 3.7）。README 与 filesystem-semantics.md 大纲补
  "gc 并发""空仓库"内容（见 §4）。
