# RFC 0000 — gitfs：将 git 仓库挂载为只读 FUSE 文件系统

| 字段 | 值 |
|---|---|
| RFC | 0000 |
| 类别 | 项目规范（内部 Standards Track） |
| 标题 | gitfs — read-only git-to-FUSE 文件系统 |
| 状态 | Accepted（定稿，语义冻结） |
| 日期 | 2026-09-24（评审通过）；2026-09-25（压缩定稿） |
| 目标版本 | 0.1.0 |
| 语言 | 简体中文；要求用语依 BCP 14 [BCP14]（§2.1） |
| 讨论与勘误 | git 仓库提交历史（rfc/0000-gitfs.md 的 git log） |
| 版权 | 本文与项目代码同以 GPL-3.0-or-later 发布（§5 的 LICENSE） |

**状态说明**：2026-09-24 评审通过（决议 Q1–Q7）及其后十九轮
review（Q8–Q26 评审与勘误、Q27–Q39 定稿后复核钉缝）的全部终局
口径已并入正文，逐条决议索引见附录 A；本压缩定稿不改变任何已决
语义。逐轮勘误链、round 编号与提交拆分等过程记录见 git 提交历史
（10f0dce…db3a576）。

**规范性声明**：§2（术语与要求用语）、§3（详细设计）为规范性内
容，§4（安全考虑）为规范性的安全分析；§5–§7 为工程实践与规划、
附录 A 为过程性索引（资料性）。摘要仅作概述，不单独构成规范依据。

## 摘要

`mount.gitfs`（C++17，安装于 `$(sbindir)`，遵循 mount(8) 助手命名
约定 `mount.<fstype>`）把一个本地 bare 或普通 git 仓库挂载为**只读**
FUSE 文件系统。三种等价调用：`mount -t gitfs <repo> <dir>`、
`/etc/fstab` 条目、直连 `mount.gitfs <repo> <dir>`。挂载点根目录
暴露（图 1）：

```
/mnt/gitfs/
├── branch/      # 本地分支
├── tag/         # tag（annotated 与 lightweight）
├── commit/      # 任意 commit（仅按完整 oid 访问，不可枚举；
│                #   oid = git 对象的十六进制名）
├── remote/      # 远端跟踪分支 <remote>/<branch>
├── HEAD/        # 当前 HEAD 指向的快照
├── commits      # 只读文件：全部可达 commit 的完整 oid 清单
└── .gitfs.json  # 挂载元信息
```

每个 ref（git 的命名引用——分支、tag、远端跟踪分支或 HEAD）或
commit 解析为一个 commit 对象，
其 root tree 以真实目录树呈现，无需 `checkout` 即可用普通工具
（`ls`、`cat`、`grep`、diff 工具）浏览任意历史版本的文件内容。

技术选型：**libgit2**（git 纯 C 库）+ **libfuse3**（FUSE——用户态
文件系统框架——的用户态实现）+ CMake 构建。

## 目录

- 摘要
- 1. 引言（1.1 动机 / 1.2 目标 / 1.3 非目标）
- 2. 术语与要求用语（2.1 要求用语（BCP 14）/ 2.2 git 侧名词 /
  2.3 依赖名词 / 2.4 本文铸造名词）
- 3. 详细设计：3.1 路径与对象映射 · 3.2 权限与元数据映射 ·
  3.3 FUSE 操作实现 · 3.4 进程模型与并发 · 3.5 缓存与性能 ·
  3.6 根目录元信息 · 3.7 CLI 与退出码
- 4. 安全考虑
- 5. 工程结构（开源最佳实践）
- 6. 备选方案
- 7. 里程碑
- 8. 参考文献（8.1 规范性 / 8.2 资料性）
- 附录 A. 评审决议索引（资料性）

## 1. 引言

### 1.1 动机

- 在多个版本之间快速对比、检索，而不必反复 `git worktree add` /
  `git archive`。
- 只读语义天然安全：任何工具对该挂载的写入都会得到 `EROFS`，不可
  能污染仓库。
- 现有方案（如 presslabs/gitfs）主要面向"当前分支 + 历史"，或绑定
  特定云存储；需要一个轻量、语义清晰、C++ 实现、符合开源工程规范
  的基础组件。

### 1.2 目标（v0.1）

1. 只读文件系统；所有写操作返回 `EROFS`，挂载参数强制 `ro`。
2. 根目录暴露五个入口目录（`branch/`、`tag/`、`remote/`、
   `commit/`、`HEAD/`）与 `commits`、`.gitfs.json` 两个合成文件
   （gitfs 自行生成的固定名文件，见 §2.4）。
3. 完整呈现 git tree：目录（`040000`）、普通文件（`100644`）、可
   执行文件（`100755`）、符号链接（`120000`）；子模块（`160000`）
   以空目录 + 说明文件占位。
4. 对普通文件支持 `read`/`mmap`（不可变 blob，天然适合内核页缓存）。
5. 单机 Linux（fuse3），C++17，依赖仅 libgit2（≥1.4）+ libfuse3
   （≥3.10），CI 矩阵验证。
6. 完整开源工程配套：CMake、单测+集成测试、CI、文档、规范提交。
7. 以 mount(8) 助手形态（§2.3）与系统集成：`mount -t gitfs` 与
   fstab 条目可直接使用。

### 1.3 非目标（v0.1，未来另立 RFC）

- 任何写路径（写入、暂存、提交）。
- 网络/远端操作（fetch/push/clone），不访问网络。
- 稀疏检出、部分克隆（partial clone）。
- macOS（osxfuse/macFUSE）与 BSD 移植（接口层预留抽象）。
- 工作区（index）视图、stash 视图。

## 2. 术语与要求用语

### 2.1 要求用语（BCP 14）

本文要求用语依 BCP 14 [BCP14] 解释，中文对应如下：**必须**
（MUST）与**禁止**（MUST NOT）为绝对要求；**应当**（SHOULD）与
**不应当**（SHOULD NOT）为在权衡全部因素后、存在正当理由时可以
偏离的强烈建议；**可以**（MAY）为真正可选。正文的钉住口径多以
"一律/恒/不得/永不/仅/不支持/钉住"等明示语气表达，均属"必须"
级；以"应当/不应当/可以/建议/留作"明示的语句方按对应级别解释。

### 2.2 git 侧名词

（按 libgit2 [LIBGIT2] 与 git 文档 [GIT] 的语义使用；本文假定读者
了解 git 基本概念）：

- **对象 / oid**：git 内容寻址存储（**ODB**）中的四类对象——commit、
  tree、blob、annotated tag；**oid** 为其十六进制名（sha1=40 位、
  sha256=64 位）。**packfile / loose** 为 ODB 的两种物理存放形态；
  **alternates** 为 ODB 指向外部共享存储的机制。
- **ref / 符号引用**：`refs/` 命名空间下的命名引用（分支
  refs/heads/…、tag refs/tags/…、远端跟踪 ref refs/remotes/…皆为
  ref）；**符号引用**（如 refs/remotes/\<r\>/HEAD）指向另一 ref 而
  非对象。**refdb** 为 ref 数据库，**packed-refs** 为其磁盘紧凑
  存储；`git update-ref` 与手改 packed-refs 是两种写入路径。ref 名
  受 `git check-ref-format` 校验（禁空格、控制字符及 `~^:?*[\`）。
- **D/F 规则**：refdb 在正常写入路径下保证一个 ref 名不会是另一个
  ref 名的前缀（`refs/a` 与 `refs/a/b` 不能并存）；手改 packed-refs
  可破坏之。
- **peel**：沿 annotated tag 链下行至非 tag 对象；本文"peel 至
  commit"即解析 ref/tag 的最终 commit 目标。
- **root tree / tree entry**：commit 指向的顶层 tree 及其子条目
  （名 + mode + oid），mode 取值见 3.2 表。tree entry 名可为任意
  字节（不受 check-ref-format 约束）。
- **revwalk / rev-list**：自一组起点按祖先关系遍历 commit 的机制，
  `git rev-list --all` 为其命令行形态；**拓扑序**为按祖先关系约束
  的出队顺序。
- **unborn / detached HEAD**：HEAD 指向不存在的分支（空仓库）/
  HEAD 直指 oid 而非分支。
- **fsck**：git 对象完整性检查；手工 `hash-object -w`/`mktree` 可
  绕过它构造非法对象。
- **worktree / index**：工作区与暂存区，v0.1 不呈现（§1.3）。

### 2.3 依赖名词

**libgit2**（git 纯 C 库，含线程与缓存开关）、
**libfuse3 / FUSE**（用户态文件系统框架及其内核接口
[FUSE3]）、**mount(8)
助手**（命名 `mount.<fstype>`、由 mount(8) 在挂载该 fstype 时 exec
的程序，契约见 3.7 [MANPAGES]）、**PATH_MAX / NAME_MAX**
（内核路径/文件名长度
上限 [POSIX]）。

### 2.4 本文铸造名词

（本节各条即定义；正文首用处不再重复）：

- **入口目录**：根下的 `/branch`、`/tag`、`/remote`、`/commit`、
  `/HEAD` 五个固定目录。
- **合成文件 / 合成项**：gitfs 生成而非来自 git tree 的文件
  （`commits`、`.gitfs.json`、`.gitfs-submodule`）及 readdir 中
  对应的条目。
- **分组前缀节点**：嵌套 ref 名（如 `feature/foo`）按首分量分组
  渲染时产生的合成命名空间目录（`/branch/feature`、`/tag/v1.0`、
  `/remote/origin`）；不含任何 tree 条目。
- **完整 ref 节点**：解析到一个完整 ref 的目录，即该 ref 目标
  commit 的 root tree（元数据按 tree 目录口径）。
- **合并节点**：既是完整 ref 又含子层 ref 的路径（`refs/tags/foo`
  与 `refs/tags/foo/bar` 并存时的 `/tag/foo`）。
- **折叠**：readdir 列单中两个同名来源（父目录列单的完整 ref 项
  与子 ref 分组项；合并节点列单的 tree 条目与子 ref 名）合并为一
  项、按子 ref 渲染为目录。
- **裸命名空间 ref**：`git update-ref` 直写 `refs/remotes/<x>` 本体
  且其下无任何子层 ref 的形态。
- **隐藏 HEAD**：`refs/remotes/<remote>/HEAD` 符号引用在枚举与查找
  中均被跳过的规则。
- **header-only 读取**：`git_odb_read_header` 只读对象头取尺寸、
  不触发 blob 全量解压的路径。
- **超限 blob**：单个 blob 尺寸 ≥ 当前 `--blob-cache-size` 的 blob。
- **open-pin**：open 时一次性完成查找与准备、结果钉住于 `fi->fh`
  供本次 open 的全部 read 使用、release 时释放的模式。
- **单互斥**：串行化全部 libgit2 调用的那一个 `std::mutex`（3.4）。
- **解压计数**：每次 blob 全量解压记一条 verbose 日志的可观测钩子。
- **指纹**：`commits` 清单的失效键——全部 ref 指向集合加 HEAD 指向。
- **零解压断言**：§5 集成测试中"`-v` 挂载下遍历目录不产生解压
  日志"的断言（目录限定语见 §5）。
- **枚举轨 / 表轨**：到达 `-o` 串键的两条分诊轨道——实测枚举名单
  / man mount(8) "Filesystem-independent mount options" 表（3.7）。

## 3. 详细设计

### 3.1 路径与对象映射

路径按**分量（component）逐级映射到 git 对象**，绝不拼接字符串后
`realpath`，因此不存在路径穿越问题：

```
/                        → 根（固定入口 + 合成文件，见 3.6）
/branch                  → 枚举本地分支（git_branch_iterator）
/branch/<name>           → 分支 tip commit 的 root tree
/branch/<name>/a/b.txt   → tree 递归下行
/tag/<name>              → tag peel 至 commit 的 root tree；peel 失败
                           （直接指向 tree/blob）→ ENOENT 并记警告
/remote                  → 枚举 refs/remotes/ 下实际存在的首层命名空间
                           （refdb 直读；不用 git_remote_list——后者仅
                           列配置项，可能与残留跟踪 ref 不一致）
/remote/<remote>/<b>     → refs/remotes/<remote>/<b> 的 root tree；
                           remote 名不含 /，路径在首分量切分、其余
                           整体为分支路径，无歧义
/HEAD                    → 当前 HEAD commit 的 root tree（unborn →
                           ENOENT）
/commit/<full-oid>       → 该 commit 的 root tree；仅接受完整**小写**
                           十六进制 oid（sha1=40、sha256=64；大写与
                           非法字符 → ENOENT，与 commits 清单的小写
                           输出一致）；oid 须指向 commit 对象本身、
                           不做 peel（tag/tree/blob 的 oid → ENOENT）
/commits                 → 只读文件：全部可达 commit 的完整 oid 清单
                           （生成与排序见 3.5）；不支持短前缀解析，
                           用户自行 `grep ^<前缀> commits` 检索
```

- `/commit` 的 readdir **恒为空**（不可枚举）：巨型仓库（linux 内核
  ~110 万 commit）在 readdir 的 offset 续读语义下要么缓存 ~100MB
  条目、要么 O(n²) 重走 revwalk，且首次全量遍历耗时数秒会挂起列
  目录进程；oid 检索改由 `commits` 清单承担（首次 open 生成、整块
  缓冲、按 offset 切片读，见 3.5）。
- ref 名受 check-ref-format 约束（§2.2），天然是合法文件名；可含
  Unicode（如中文），直接作为文件名。
- **ref 目标非 commit 的统一口径**：`/tag`、`/branch`、`/remote`
  与 `/HEAD` 的解析一律 **peel 至 commit** 后取 root tree；peel
  失败——ref 直接指向 blob/tree（除 tag 外，分支、远端跟踪 ref 与
  HEAD 经手工 `git update-ref` 同样可置于该形态，git 均允许）——
  一律 `ENOENT` 并记警告日志，v0.1 不呈现非 commit 目标。`commits`
  清单生成对同类 ref 则跳过、不算错误（3.5）：入口不可见、清单不
  受拖累，两处口径互补。
- **tree entry 名不受 check-ref-format 约束**，可为任意字节：git
  对象格式保证 entry 名不含 `/`（含 `/` 会被拆成嵌套 tree）与 NUL
  （tree 条目的格式分隔符），因此**按原始字节透传**给 VFS 即安全，
  不做任何改写/转义；超长仍按 NAME_MAX → `ENAMETOOLONG`（3.3）。
  非 UTF-8 名字在用户侧显示为乱码属预期，与 `git checkout` 一致。
  **唯一例外 `.`/`..`**：手工构造（绕过 fsck）的对象可产生这类
  entry，与 VFS 保留名冲突，readdir/getattr **跳过**（getattr →
  `ENOENT`）并记警告日志；fuse3 的 `.`/`..` 由内核生成，readdir
  亦不得返回。
- **`refs/replace` 不生效（透传原始对象）**：语义等同
  `git --no-replace-objects`——`/commit/<oid>` 的内容寻址承诺
  （oid ↔ 对象一一对应）优先于替换机制；代价是在设置过 replace
  refs 的仓库中输出与默认配置的 `git cat-file`/`git ls-tree` 不同
  （§5 测试 oracle 一律带 `--no-replace-objects`）。libgit2（截至
  1.9）本就不实现 replace refs，透传即其默认行为；若未来版本引入
  遵循开关，构建时显式关闭并加单测防回归。
- **含 `/` 的 ref 名（分支 `feature/foo`、tag `v1.0/rc`、远端
  `origin/feature/x`）按嵌套目录渲染**：readdir 按首分量分组；查找
  时逐级累积分量查询 refdb。D/F 规则保证至多存在一个完整 ref 名
  前缀匹配、剩余分量即 tree 路径，无歧义——该保证只在正常写入
  路径成立，手改 packed-refs 可破坏之（同时含 `refs/tags/foo` 与
  `refs/tags/foo/bar`），解析回退钉住：查找取**最长 ref 名匹配**
  （`/tag/foo/bar` 解析为 ref `refs/tags/foo/bar` 的 root tree，
  而非 ref `foo` 的 root tree 下行 entry `bar`；branch/remote
  同构）。**折叠**命中歧义记警告日志（病态仓库防御，与 tree
  entry `.`/`..` 的手工构造防御同向，正常 refdb 路径不触发）。
- **合并节点自身的 readdir**：枚举定为该 ref root tree 条目 ∪ 子
  ref 名的**并集**——子 ref 名按**首分量**参与并集（多分量子 ref
  如 `refs/tags/foo/feature/x` 以 `feature` 入列、其下再按嵌套 ref
  规则分组渲染），同名（tree 恰含 entry `bar` 且子 ref 亦名
  `bar`）按子 ref 折叠为目录——与父目录折叠口径一致。若该 ref 为
  非 commit 目标，节点按统一口径 lookup → `ENOENT`，而父目录
  readdir 仍渲染折叠目录项，形成形状失配——**以查找口径为准**
  （目录项仅为枚举线索、不构成可访问性承诺），失配同样记警告
  日志。
- **完整 ref 节点**即其 root tree（元数据按 tree 目录口径，mtime
  = 所属 commit 的 committer time，见 3.2）；**分组前缀节点**为
  合成目录：`S_IFDIR|0755`、`nlink=2`、时间戳 = 挂载时刻（与入口
  目录同口径，见 3.2），非 tree 目录、不取 committer time。
- **`/remote` 特例**：`refs/remotes/<remote>/HEAD` 符号引用按
  **隐藏 HEAD** 规则处理（访问 → ENOENT）——peel 后仅是默认分支
  树的重复，符号语义也无法在目录树中表达；非符号形态（update-ref
  直写 oid）不属隐藏规则、按普通跟踪 ref 处理——peel 至 commit
  呈现，非 commit 目标同统一口径 ENOENT。仅含该隐藏 HEAD 的命名
  空间仍渲染为**空目录**、不抑制（命名空间存在性以 refdb 为准，
  跟踪 ref 出现后由实时枚举自动填充）。**裸命名空间 ref**按命名
  空间目录处理、本体不呈现：该 ref 的 commit 快照经 `/remote`
  隐式不可达——remote 入口恒按 `<remote>/<分支>` 两层切分的附带
  后果（与隐藏 HEAD 的刻意不可达不同类，声明性行为而非缺陷）；
  该隐式不可达仅对裸形态成立——子层 ref 一旦经手改 packed-refs
  与之并存，即转入合并节点口径：`/remote/<x>` 枚举为并集、本体
  经查找恢复可达。`commits` 清单仍纳入该 ref（入集与呈现口径
  无涉，见 3.5）。
- **readdir 顺序全域钉住**：根、`branch/tag/remote`（含分组前缀
  目录）、tree 目录与合并节点并集一律按**路径分量原始字节
  （memcmp，无 locale 参与）字典序**输出；并集中子 ref 名这类非
  tree 条目与 tree 条目混入同一排序、不按来源分组。tree 目录
  **重排**为字节字典序而非沿用 git tree 的内在条目序——后者按
  "目录名附加 `/` 后参与比较"的规则，在 `foo`（目录）与
  `foo.txt`（文件）这类组合上与纯字节序相反（`0x2E '.'` <
  `0x2F '/'`，git 序把 `foo.txt` 排在 `foo/` 之前）——换取整个
  挂载统一、可复现、不依赖 git 内部排序实现的枚举顺序；
  `.gitfs-submodule` 合成项按其文件名参与同一排序；`.`/`..` 不由
  gitfs 返回。集成测试直接断言字节序，或以 `LC_ALL=C sort` 归一
  后与 `git ls-tree` 比对集合（§5）。
- `/branch`、`/tag`、`/remote` 的 readdir 实时反映仓库外部更新
  （`/commit` 恒空，见上），新 commit、新 tag 无需重新挂载即可见；
  已解析的旧对象只要仍存在于 ODB 中就继续可访问（配合 3.5 缓存）。
- **空仓库**（无任何 refs 且 unborn HEAD）：挂载照常成功——
  `/branch`、`/tag`、`/remote` 为空目录，`/HEAD` 访问 → ENOENT，
  `commits` 为空清单（`st_size=0`，open 生成空缓冲），
  `.gitfs.json` 的 `head` 为 `null`（3.6）。
- **挂载期间的外部 `git gc`/`git prune`**：gitfs 不加锁、不阻止
  任何外部 git 操作。若 gc 重写 pack 或 prune 删除了后续请求仍需
  要的对象，该请求瞬时返回 `ENOENT`（对象消失）或 `EIO`（pack
  中途失效），gc 结束后即恢复；gitfs 不承诺挂载期内对象集不变
  （`commits` 清单生成期间命中同一口径，见 3.5）。运维建议：挂载
  期间禁用自动 gc（`git config gc.auto 0`）或接受上述瞬态错误
  （README 与 filesystem-semantics.md 中声明，见 §5）。

### 3.2 权限与元数据映射

表 1：git entry mode → st_mode 映射

| git entry mode | st_mode | 说明 |
|---|---|---|
| `040000` | `S_IFDIR \| 0755` | 目录 |
| `100644` | `S_IFREG \| 0644` | 普通文件 |
| `100755` | `S_IFREG \| 0755` | 可执行 |
| `120000` | `S_IFLNK \| 0777` | 符号链接，readlink 返回 blob 内容 |
| `160000` | `S_IFDIR \| 0755`（空目录）+ 说明文件 | 子模块：渲染为空目录，旁边放 `<name>.gitfs-submodule` 文本文件（`<name>` 即该子模块目录名，如 `deps/libfoo` → `deps/libfoo.gitfs-submodule`；内容含 url 与 commit oid）；若树中已存在与该合成文件名相同的真实条目，真实条目优先、省略合成文件（记警告日志） |

- `st_size`：**普通 blob** 取原始字节数，一律经 **header-only 读取**
  获得、不触发 blob 全量解压——`getattr` 与 3.5 的超限准入判定同
  用此路径，含多 GB blob 目录的 `ls -l` 或 open 阶段的超限判定都
  不为取 size 付整块解压的代价（与 3.3 已钉住的"getattr 不触发
  revwalk"同构）。
  **symlink 例外**：st_size 取**截断至首个 NUL 后的长度**，与
  readlink 口径对齐——stat 报告的尺寸恒等于 readlink 返回的字节
  数，含嵌入 NUL 的 symlink 不得报原始 blob 尺寸（取该值须读
  blob 内容定位首个 NUL，为 header-only 路径的显式例外；symlink
  blob 上限 PATH_MAX、内容本就须为 readlink 读取，代价可忽略——
  该上限只对 git 正常写入成立）。
  **病理形态的显式例外**：截断后长度 ≥ PATH_MAX 的手工 symlink
  blob（`hash-object` 手工构造，git 正常写入不产生；含 NUL 与否
  均在此例外内）——无 NUL 形态全长即截断长度、st_size 报全长
  （取全长须 getattr 全量扫描 blob）；NUL 位于 PATH_MAX 之后的
  形态（如 5000 字节、首 NUL 在 4500）截断长度 4500 ≥ PATH_MAX、
  st_size 报 4500（读至首个 NUL 即止）。两种形态 readlink 均报
  `ENAMETOOLONG`（3.3），"stat 尺寸 = readlink 返回字节数"的不变
  式在此病理形态显式失效（st_size 不回退为 0 或其他值）；"代价
  可忽略"的论证只锚定正常写入、对此形态失效。git 正常写入不产
  生的病态形态，不为其优化。
- `st_mtime`/`st_ctime`：所属 commit 的 committer time（同一快照
  内全部一致，便于 rsync 类工具判断）。`st_atime` 与之同值、不随
  读取更新——只读快照语义，避免逐次 stat 漂移（主流发行版
  relatime 下内核本就不强制刷新 atime，用户无感知）。
- `st_nlink`：目录为 2，文件为 1（简化，不做精确统计）。
- `st_blocks = ceil(st_size/512)`（`commits`/`.gitfs.json`/
  `.gitfs-submodule` 等合成文件同口径，按其内容字节数计——
  libfuse 不按 st_size 自动推导，缺省 0 会让 `du` 对全部文件报
  0 块）；`st_blksize = 4096`（与 statfs 汇报的块大小一致）；
  `st_rdev = 0`（任何 entry 均非设备节点）。
- `st_ino`：由完整 VFS 路径字节的 64 位稳定哈希派生（合成入口同
  口径，根固定为 FUSE_ROOT_ID=1），跨重挂载确定。**刻意不做 oid
  派生**——同一 blob 出现在多个 ref 路径下若共享 inode，会被
  `tar -c`/`rsync -H` 误判为硬链接；且 inode 相等**不**承诺内容
  同一（本文件系统无硬链接语义）。**碰撞消歧**：哈希函数可注入
  （单测可强制构造碰撞）；挂载期维护 `primary_hash → 首个占用
  路径`注册表（受 3.4 单互斥保护），后到路径命中已占用哈希时依次
  取 `H(path || '#' || k)`（k=1,2,…）中最小的未占用值——不同路
  径必得不同 inode；跨重挂载可复现性在冲突路径上除外（10⁶ 条目
  生日碰撞概率 ≈ 3×10⁻⁸，注册表是正确性兜底而非预期路径）。条
  目**懒注册**——仅 getattr/readdir 实际解析过的路径才入表，不做
  全仓库预注册；挂载期内**不回收**（inode 号永不复用，规避"同号
  异对象"对 `find -inum`、NFS 句柄类工具的误导）；占用上界 ∝
  挂载期内被触及的**不同** VFS 路径数（每条 ≈ 路径字节数 + 数十
  字节开销，百万级触及 ≈ 数十 MB 量级），README 与
  filesystem-semantics.md 中声明（§5）。挂载基线因此含 `use_ino`
  （3.7）。
- `st_uid`/`st_gid`：挂载进程的 uid/gid（fuse 默认行为）。
- **合成文件与目录的元数据**：`commits` 与 `.gitfs.json` 为
  `S_IFREG | 0444`、`nlink=1`（写路径本就 EROFS，`cp` 类工具按只
  读源处理）；根、五个入口目录与分组前缀节点均为 `S_IFDIR|0755`、
  `nlink=2`、`st_mtime/ctime/atime` = **挂载时刻**（入口集合虽实
  时枚举，时间戳钉住挂载快照不漂移，与 `.gitfs.json` 同口径）；
  **目录 `st_size` 恒为 4096**（根、入口、分组与 tree 目录统一，
  合并节点经完整 ref 节点同归 tree 目录口径；与 `st_blksize` 同
  量级的目录尺寸惯例值）。`commits` 的 `st_mtime`：**生成前**
  （`st_size=0` 窗口期）为挂载时刻（不逐次取当前时钟，避免逐次
  stat 漂移），首次 open 后跳变为真实生成时刻（与 size 0→真实值
  同构）；指纹变化触发重建后，stat 报**当前缓存版本**的生成时刻
  ——正在钉住旧版本读的句柄可能看到比自己快照更新的 mtime，与
  3.5"下一次 open 起可见新版本"口径一致。`.gitfs-submodule` 为
  所属 commit 的 committer time（内容确定性派生自树）；`.gitfs.json`
  为挂载时刻（3.6）。
- **`.gitfs-submodule` 说明文件**：mode `0644`，内容为两行
  `key=value` 文本——`url=<submodule url>` 与 `commit=<完整 oid>`
  （各以 LF 结尾）；url 取自该 commit 树根 `.gitmodules` 中对应
  path 的条目，缺失或无对应条目时 `url=` 置空并记警告日志。
  **`.gitmodules` 读取路径**：按普通 blob 走 3.5 的 LRU 路径
  （命中纯内存、miss 锁分段装载并记一条 verbose 解压日志），不另
  设旁路——LRU 即其备忘层（oid 键控、blob 不可变，挂载期内无失
  效问题），解析 path→url 为内存操作，不为逐 commit/逐 entry 另
  设备忘层；病态超限（≥ `--blob-cache-size`）的 `.gitmodules`
  不得入池、每次调用各解压一次各记一条日志（绕过分支，见 3.5）
  ——合成条目 st_size 按内容字节数计、无 header-only 捷径，与病
  理 symlink 的全量扫描同款：git 正常写入不产生的病态形态，不为
  其优化。

### 3.3 FUSE 操作实现

表 2：FUSE 操作语义

| 操作 | 行为 |
|---|---|
| `getattr` | 路径 → 对象（3.1），失败 `ENOENT`；`commits` 恒为纯缓存读（未生成时 `st_size=0`），**不**触发 revwalk 或指纹重算（3.5）；**普通 blob** 的 `st_size` 经 header-only 读取获得，**不**触发 blob 全量解压（symlink 为显式例外——取截断长度须读内容定位首个 NUL，无 NUL 病理形态须全量扫描，见 3.2） |
| `readdir` | 根：固定列表；`branch/tag/remote`：枚举 ref（含 `/` 的名字按目录分组）；`commit`：**恒为空**；tree：枚举 entries（含 `.gitfs-submodule` 合成项）；合并节点：root tree 条目 ∪ 子 ref 名的并集、同名按子 ref 折叠（3.1）——全部目录（含根，并集含其中）的输出顺序一律按分量原始字节字典序、子 ref 名与 tree 条目混排不分组（3.1）。注册 `opendir/releasedir`：枚举列表快照挂于 `fi->fh`，保证单目录流内 offset 续读稳定（fuse3 要求），跨目录流实时反映 ref 变化 |
| `open`/`release` | 仅校验 `O_RDONLY` 系标志（写标志 → `EROFS`，与内核对 ro 挂载的判定一致）；`commits` 首次 `open` 触发生成（3.5），且把当前缓冲版本**钉住于 `fi->fh`**——同一次 open 的所有 read 分片读自同一快照，refs 中途变化不影响（与 readdir 的目录流快照同构），`release` 时解除钉住；超限 blob 的 `open` 同构 **open-pin**：一次性 lookup（单互斥内）+ 全量解压（锁外执行，不阻塞全挂载其他请求），解压块钉住于 `fi->fh`、`release` 释放（3.5）；`.gitfs.json` 挂载期内不可变，无需钉住 |
| `read` | 定位 blob（可缓存者经 LRU 缓存；超限者读 open 时钉住的解压块，见 3.5），拷贝 `[offset, offset+size)` 越界截断；合成文件（`commits`、`.gitfs.json`、`.gitfs-submodule`）为整块只读缓冲，`commits` 读 open 时钉住的版本 |
| `readlink` | symlink blob 内容；内容含嵌入 NUL 时**截断至首个 NUL**（内核 symlink 目标不可含 NUL；与 `git checkout` 的事实行为一致，显式同语义而非 `EIO`）；截断口径与 `st_size` 对齐——symlink 的 stat 尺寸即截断后长度（3.2）；空 blob → 返回长度 0 的空目标；**截断后长度 ≥ PATH_MAX → `ENAMETOOLONG`**（内核接受的目标长度上限为 PATH_MAX−1、恰等于 PATH_MAX 即失败——含 NUL 与否同判；此病理形态下 st_size 仍按截断口径报长度，"stat 尺寸 = readlink 字节数"不变式显式失效，见 3.2） |
| `statfs` | 汇报本地 ODB 占用为 `f_blocks`（全部 packfile 字节 + loose 对象字节；alternates 指向的外部存储不计入，启用 alternates 时 verbose 日志提示），块大小 4KiB；`f_bfree = f_bavail = 0`——只读卷惯例是 0 空闲，`df` 显示 100% 已用，向用户明确传达"无任何可写空间"（若报全量可用，`df` 会显示 0% 已用，易误导）；`f_files = f_ffree = 0`（精确 inode 计数需全量遍历，v0.1 不承诺，内核与 `df` 均容忍 0） |
| 其余（`mknod/mkdir/write/…`） | 返回 `EROFS` 或不注册（fuse3 只读挂载兜底）；**xattr 族**（get/set/list/remove）一律不注册 → libfuse 缺省 `ENOSYS`，内核标记"无 xattr"后统一向用户态报 `ENOTSUP`（SELinux 等环境的 `security.*`/statx 附加字段查询命中此路径，干净短路而非逐次回环） |

错误映射：libgit2 错误码 → `ENOENT`（对象/分支不存在；oid 格式非
法也统一 `ENOENT`，不向调用方暴露内部规则）、`EIO`（ODB 损坏）、
`ENAMETOOLONG`（文件名超过 NAME_MAX）等，集中在一个 `git_to_errno()`
翻译函数。readlink 的 NUL 截断/空目标（见上表）是**非错误**的降级
语义，同样在该翻译层旁注明，避免实现时误报 `EIO`。

### 3.4 进程模型与并发

- v0.1 以 **fuse3 多线程模式** 运行，但所有 libgit2 调用集中在
  `GitRepo` 包装类内，受**单互斥**串行化（libgit2 全局线程需
  `git_libgit2_init`，且 1.x 对同 repository 的并发只读有保证，
  仍保守串行化——性能瓶颈在 I/O 而非锁）。
- **让锁例外一——revwalk 分 chunk**：`commits` 生成的 revwalk 每
  批 `git_revwalk_next` 一定数量（如 4096）后释放单互斥让其他请求
  插队，随后重取继续，walker 本身仍归生成线程独占（revwalk 句柄
  非线程安全）；释放窗口内其他线程只做同 repository 的并发只读
  操作，libgit2 1.x 已保证安全。单 chunk 持锁时间为毫秒级，交互
  请求延迟不可感知；清单总生成耗时数量级不变。**并发首次 open
  以 single-flight 串行化**：生成由一次性互斥（once/双检锁）保护
  ——首个触发线程在锁内重查"已生成且指纹一致"后才启动 revwalk，
  其余并发 open 阻塞等待同一次生成完成后直接复用缓冲；不做两个
  revwalk 同时跑的浪费，也不存在缓冲交换竞态。
- **让锁例外二——超限 blob 全量解压在锁外**（3.5）：open 时单互
  斥内仅做 `git_blob` lookup/句柄获取，全量解压在**锁外**执行
  ——解压块只归该 open 的句柄所有、无共享可变状态（让锁窗口内
  其他线程只做同 repository 并发只读，同一安全依据）；避免数 GB
  blob 解压期间（数秒）阻塞全挂载。该 open 自身的返回时间仍与
  全量解压同阶（调用方需预期，README 声明）。
- **让锁例外三——可缓存 blob 的 LRU 装载同构分段**（3.5）：
  `--blob-cache-size` 无人为上限（3.7），用户调大后接近上限的单个
  可缓存 blob 若整段在锁内装载（含解压数秒），恰好重现例外二专
  门要避免的全挂载停摆——故与超限 open-pin 同构分段：**锁内**
  lookup（命中即返回）并标记装载意图，**锁外**完成对象读取与解
  压（解压块归装载线程私有、无共享可变状态），**回锁复查**——
  期间他线程已插入同一 oid 则丢弃自家块复用缓存项，否则插入 LRU
  （插入时容量不足则整条不入池，退化为本次调用持有私有块的"绕过"
  路径，与超限语义合流）。代价是同一 miss 的并发首载可能**瞬时
  重复解压**（败者块即刻丢弃），不承诺严格 single-flight——瞬时
  内存上界 = Σ 各并发装载线程所持解压块大小（并发数有限且败者块
  即刻释放），远优于让全挂载为单个大可缓存 blob 停摆数秒；正常
  工作集下重复窗口为毫秒级。锁内剩余工作（lookup、LRU 逐出与
  记账）为微秒级——分段后单互斥内不再有毫秒级以上的整块解压长
  持锁，让锁动机全域对称。
- RAII 封装所有 libgit2 句柄（`git_repository`、`git_tree`、
  `git_blob`…），自定义 deleter 的 `std::unique_ptr` 别名，异常
  安全，热路径无异常。
- `SIGINT`/`SIGTERM` → `fuse_session_exit` 优雅卸载；同时支持
  `umount` 外部卸载。

### 3.5 缓存与性能

- **blob LRU 缓存**：键 = blob oid，值 = 不可变字节串；容量按字节
  计，默认 64 MiB，`--blob-cache-size` 可调（无人为上限，3.7）。
  命中则 `read` 为纯内存拷贝；miss 装载按 3.4 的锁分段执行，解压
  记一条 verbose 日志（解压计数）。
- **超限 blob 绕过缓存、open-pin 一次性解压**：不插入缓存、也不
  触发既有条目逐出（为单个超限对象清空整池只会引起缓存抖动；边
  界刻意取 `>=` 而非 `>`——恰等于容量上限的 blob 若尝试入池，会
  为容纳它逐出整池其余条目、自身又几乎占满全池，等效清空整池，
  与绕过动机直接矛盾）。超限判定所需的 blob 尺寸经 header-only
  读取获得（3.2），`open` 的准入判定不因取 size 触发全量解压。
  解压时机为与 `commits` 同构的 **open-pin**（3.3）：`open` 时完
  成一次 `git_blob` lookup 与**全量解压**（锁边界见 3.4），解压
  后字节块钉住于 `fi->fh`，该 open 内所有 read 分片直接自该块切
  片拷贝，`release` 时释放——单次 open 的顺序分片读**只解压一
  次**（逐 read 重做 lookup+全量解压的字面直读会使大 blob 顺序
  分片读呈 O(size²) 解压放大——blob per-type 缓存已写死 0（见
  下）、自家 LRU 又不收超限对象、且解压结果不在内核页缓存——页
  缓存只保留 mmap 的 pack 原始字节——"页缓存兜底"并不能消除重复
  解压，故不采用）。**内存峰值（显式声明）**：解压块不进共享缓
  存、按 open 独立持有，峰值 = Σ 各未 `release` 的超限 blob open
  所持 blob 大小（并发打开同一超限 blob 即"并发 open 数 × blob
  大小"量级），由调用方自行控制；v0.1 不做跨 open 引用计数共
  享，流式按需解压（解压块不整块驻留）留作后续优化。open 时装
  载失败的错误按 3.3 映射返回（对象被外部 gc 删除 → `ENOENT`、
  pack 中途失效 → `EIO`），不产生钉住句柄。**每次全量解压记一条
  verbose 日志（解压计数）**——超限 open-pin 与缓存 miss 装载各
  一条，为"只解压一次"提供直接可观测钩子（§5 断言用）。
- **tree/commit 依赖 libgit2 内置对象缓存，init 时显式调参**：默
  认 per-type 上限仅 4KiB（源码 `cache.c` 的
  `git_cache__max_object_size[]`），序列化超线的大目录 tree（~100+
  entry 即超）不进缓存；而高层 fuse API 每个 syscall 自 root tree
  重解析 O(depth) 次、attr/entry_timeout 又显式置 0（见下），tree
  命中率就是 `ls -R`/`find`/`du`/rsync 类元数据负载性能的全部。
  故 init 时以 `GIT_OPT_SET_CACHE_OBJECT_LIMIT(GIT_OBJECT_TREE,
  1MiB)` 抬线，COMMIT 同抬（commit 天然 <4KiB，仅防御病态巨型
  merge commit，无代价）；总预算 `GIT_OPT_SET_CACHE_MAX_SIZE` 设
  为 `--tree-cache-size` 的值（默认 256MiB），与
  `--blob-cache-size` **各自独立记账、互不挤占**，校验规则相同
  （3.7）。
- **blob 保证不进 libgit2 缓存（防双层缓存）**：per-type 上限默认
  即 0（从不缓存），但这是默认值而非契约——任何一处
  `GIT_OPT_SET_CACHE_OBJECT_LIMIT(GIT_OBJECT_BLOB, n>0)` 都会让同
  一份字节在 libgit2 缓存与自家 LRU 各存一份、双份记账。故 init
  显式写死 `GIT_OPT_SET_CACHE_OBJECT_LIMIT(GIT_OBJECT_BLOB, 0)`，
  令"恰好不重复"成为"保证不重复"。pack 读入走 packfile mmap →
  内核页缓存，进程间共享、可回收，属正常分层而非堆内重复；
  `GIT_OPT_ENABLE_CACHING` 保持默认开启（libgit2 公开 API 即此，
  不存在按 odb 实例设置缓存的接口）。
- **`/commits` 清单**：**首次 `open` 时** revwalk 全量生成。
  **入集口径**：枚举 `refs/` 下**全部** ref——heads/tags/remotes
  之外，notes、stash、replace 等特殊命名空间一并纳入，对齐
  `git --no-replace-objects rev-list --all` 的 oracle（实测 git 对
  该集合同样全量枚举）；符号引用先解析至目标 ref 再处理；**另加
  HEAD**——detached HEAD 独有的 commit 也纳入，可达集 = 全部
  refs ∪ HEAD；每个目标**逐个 peel 至 commit**，peel 失败者——
  轻量 tag 指向 blob/tree（3.1 合法形态）等非 commit 目标——
  **跳过并记 verbose 日志，不算错误**（`git rev-list --all` 对此
  类 ref 亦静默跳过），绝不因个别非 commit ref 使整个清单生成失
  败或退化为 `EIO`；replace ref 以普通 ref 身份入队（其指向的替
  换 commit 若在 ODB 中即入清单），但对象读取不遵循替换——与
  3.1 的透传语义一致。整块缓存；以**指纹**为失效键，任一变化才
  重建。**拓扑序确定性的实现钉住**：walker 设
  `GIT_SORT_TOPOLOGICAL`，且入队序固定——收集到的完整 ref 名列
  表按字典序显式排序后逐个 push（不依赖 refdb 迭代器内部顺序），
  HEAD 恒最后入队；拓扑约束之外的并列 commit（互不为祖先的平行
  链）出队次序由该入队序唯一决定，清单跨进程、跨重挂载逐字节可
  复现。**生成失败的 open 语义**：revwalk 中途因对象被外部
  gc/prune 删除而失败（`git_revwalk_next` 返回错误）时，该次
  `open` 返回 `EIO`，半成品缓冲丢弃、不写入缓存、指纹不更新
  （等同"从未生成过"），single-flight 等待方收到同一失败；下一次
  `open` 从头重试，gc 结束后即成功（与 3.1 的瞬态错误口径一致）。
  revwalk 按 3.4 的分 chunk 锁策略执行。**生成与指纹比对只发生
  在 `open`**：生成前 `getattr` 报告 `st_size = 0`，故 `ls -l`/
  `stat`/文件管理器枚举不触发秒级 revwalk（与 3.1 将 `/commit`
  readdir 置空同一理由）；生成后 `st_size` 为真实值。单次 open 经
  `fi->fh` 钉住缓冲版本直至 release（3.3），跨 open 语义为"下一
  次 open 起可见新版本"；并发首次 open 的 single-flight 见 3.4。
  挂载后后台线程预生成留作后续可选优化（如 `--prewarm`），v0.1
  不做。110 万 commit ≈ 45MB 文本（sha1 口径：每行 41B 含 LF；
  sha256 仓库为 65B/行 ≈ 71.5MB——65B × 110 万），内存与 grep 均
  可接受。
- **FUSE 侧默认不开启 kernel_cache**：显式置 `attr_timeout=0、
  entry_timeout=0`（fuse3 默认值均为 1s，不显式置 0 则有 1 秒陈旧
  窗口）：ref 是可变的（分支可能被删），正确性优先；提供 `-o
  kernel_cache` 透传给高级用户在"仓库挂载期间不变化"场景下自行
  开启。澄清：`kernel_cache` **只作用于数据页缓存**（read/mmap 页
  跨 open 复用），开启它**不**改变 attr/entry timeout（仍恒 0，
  元数据每次穿透）；想要元数据陈旧窗口须另行显式传
  `attr_timeout`/`entry_timeout`（与 `kernel_cache` 相互独立，一
  致性风险自担）。blob 不可变，内核页缓存对 `mmap` 的收益不受
  影响。
- **性能门禁（比值阈值为主）**：`grep -r` 全树吞吐不低于
  `git archive | tar -x` 的 50%；冷缓存 `cat` 单文件耗时不超过同
  机 `git cat-file blob` 基线的 1.5 倍。共享 runner 墙钟抖动大，
  绝对值断言（如 <5ms）仅在自管 runner 上作参考检查并带 ±30%
  容差；比值门禁在共享 runner 上稳定执行，防性能回归。

### 3.6 根目录元信息

根目录额外暴露一个只读文件 `.gitfs.json`（名字以 `.` 开头，避免与
入口目录语义混淆；根目录不呈现仓库树、合成文件均为固定名，无命名
冲突；内容示例见图 2）：

```json
{
  "format": 1,
  "repository": "/abs/path/to/repo.git",
  "head": "refs/heads/main",
  "mounted_at": "2026-09-24T02:29:00Z",
  "cache": { "blob_bytes": 67108864, "tree_bytes": 268435456 }
}
```

- `head`：HEAD 指向的 ref 名；detached 时为完整 oid；unborn 时为
  `null`。`cache` 记录 blob/tree 双口径缓存字节数（3.5）。
- **非 UTF-8 仓库路径的转义**：JSON 文本必须是合法 UTF-8，
  `repository` 字段对仓库路径中构成**非法 UTF-8 序列**的字节按
  `%XX` 百分号转义（ASCII 与合法多字节序列原样保留），任意文件
  系统路径都能产出可被严格 JSON 解析器接受的文本。该转义**仅保证
  JSON 合法、不承诺可逆**——路径中本就存在的字面 `%XX`（ASCII
  百分号 + 两位十六进制）与转义产物在输出中不可区分，探测脚本不
  得据此逆推原始路径字节（需精确路径请用挂载参数或系统侧信息）。
- **失效语义（显式声明）**：`.gitfs.json` 是**挂载时刻的快照**，
  挂载期间不可变——`head` 字段在挂载后分支切换/HEAD 移动时**不**
  跟随更新（需探测实时 HEAD 请进入 `HEAD/` 目录或直接
  `git rev-parse`）；不参与 refs 指纹失效机制，重新挂载即刷新。

### 3.7 CLI 与退出码

程序以 **mount(8) 助手** 形态发布：可执行文件安装为
`$(sbindir)/mount.gitfs`，三种调用形态等价：

```
mount -t gitfs <repo> <mountpoint> [-o <opts>]   # 经 mount(8) exec 助手
mount <mountpoint>                                # /etc/fstab 条目触发
mount.gitfs <repo> <mountpoint> [选项]            # 直连调用
```

**mount(8) 实际转交行为**（经 util-linux 2.42.3 libmount 源码
[UTILLINUX] 并
真实 mount(8) exec 助手实测逐条核验）：助手契约为
`[-sfnv] [-N namespace] [-o options] [-t type.subtype] <src> <dir>`
（man mount(8) EXTERNAL HELPERS 节明载 [MANPAGES]）。libmount
exec 助手时：
转交短选项 `-s/-f/-n/-v`（`--fake` 转交为 `-f`、`-v` 映射至
verbose）、`--namespace` 转交为 `-N <ns>`、`-o` 选项串；`-r`/`-w`
**从不以标志形式转交**，而是并入 `-o` 串（`-r` → 追加 `ro`、
`-w` → 追加 `rw`）；且对未显式只读的调用，libmount **无条件在
`-o` 串中预置 `rw`**——裸调用 `mount -t gitfs <repo> <dir>` 实际
到达助手的 argv 为 `<repo> <dir> -o rw`，且选项恒出现在位置参数
**之后**（实测如 `<src> <dir> -f -o rw`），参数解析须容忍选项后
置（GNU getopt 式置换，否则主路径直接破）。`-o` 串内的 rw/ro 冲
突（如 `-o rw,ro`）经 mount(8) 时由 libmount 折叠为单个键（实测
后者胜），助手不会从 mount(8) 同时收到 rw 与 ro；fstab `ro` +
CLI `-o rw` 的覆盖流同样在 libmount 层裁决为单个到达键、到达后均
为无操作（`rw` 记警告），挂载恒为 ro（硬编码基线），实现者无须
为该组合写特殊解析（直连调用纵然出现 rw,ro 同串，两键亦各自为无
操作）。util-linux ≥2.35 还允许 CLI `-o` 在 fstab 选项之上增改。
**`-t` 子句**：带点 fstype（`mount -t gitfs.<x>`）由 libmount 以
`-t <type.subtype>` 整值转交（实测带点 fstype 的助手 argv 确为
`<src> <dir> -o rw -t <type.subtype>`，无点则恒不出现）；`gitfs`
无点、经 mount(8) 的路径永不收到该子句，但契约核验口径为穷尽，
故钉住处理：收到 `-t gitfs`（仅可能经直连调用出现）接受并记
verbose 一条（冗余自指），**其余任何值**（含 `gitfs.<x>` 带点形
态）为 fstype 错配、参数错误退出 1。`-n`/`-s`/`-N` 容忍并忽略
（`-s` 仅标志本身被容忍、不放松未知 `-o` 键拒绝——mount(8) 文档
语义为"忽略文件系统不支持的挂载选项"，但静默吞掉拼写错误的自有
键会掩盖配置错误，故显式不采用该放宽）；直连调用显式传 `-r`/`-w`
按未知选项处理 → 参数错误退出 1。

**`-o` 串中 VFS 键的实际到达口径**：libmount 仅滤除固定子集
（`auto/noauto/comment=/x-*/loop/offset=/sizelimit=/defaults` 及
传播键），其余 VFS 键原样到达。实测到达且对 gitfs 无操作的键
（**枚举名单**，util-linux 2.42.3 快照）：atime/noatime/relatime/
strictatime/lazytime/diratime/nodiratime、sync/async/dirsync、
exec/noexec、user/users/owner/group/nouser、symfollow/
nosymfollow、iversion/silent/loud/mand/nomand/nofs、_netdev、
nofail、acl/quiet/showexec/bsdgroups。其中后四键**不在** man
mount(8) "Filesystem-independent mount options" 表内（实测
2.42.3：quiet/showexec 属 FILESYSTEM-SPECIFIC 节的 FAT 选项、acl
属 ext/ntfs/overlay 等 fs-specific 节、bsdgroups 全 man 页零出
现），其到达系 libmount 将表外未知键当 fs-specific 数据转发所
致，故收入枚举名单而非表轨。**user 族实测附带键**：`user`/
`users` 隐式附带 `noexec,nosuid,nodev` 全三项，`owner`/`group`
仅附带 `nosuid,nodev`，`user=<name>` 不附带任何键——三者对硬编码
基线均为无操作或冗余、行为不受影响。

**到达键的分诊规则（双轨锚点）**：

1. 名单匹配按**截首个 `=` 取键名**钉住——`user=alice`、
   `nofail=1` 实测带值到达，按键名归入相应分诊、值不再校验
   （`context=` 族值含冒号，逗号切分不受影响、仅不得再按冒号拆分
   值）；util-linux 2.41+ 的 `ro=vfs`/`rw=fs` 及 recursive 参数形
   态截 `=` 后键名即 ro/rw，落入既有无操作路径（ro 冗余、rw 记警
   告），无须特殊解析；
2. **枚举轨优先**——上列名单所列键一律接受并忽略、verbose 记一
   条；
3. **表轨兜底**——仅认 man mount(8)
   "Filesystem-independent mount options" 一节**所列**的键：所列
   且对 gitfs 为无操作者（ro 基线、atime≡mtime、无属主映射语义
   下无效果；表内否定键 `noiversion`/`norelatime`/
   `nostrictatime`/`nolazytime` 与带值形态（`noexec=recursive`
   等）正是由本轨经截 = 取键名吸收——枚举名单未逐一收录它们）
   → 接受并忽略、verbose 记一条；该表中对 gitfs 有语义或安全影
   响者（`suid`/`dev`、`remount`、`uid=`/`gid=`/`umask=`、
   `context=`/`fscontext=`/`defcontext=`/`rootcontext=`（SELinux 标签
   键，值含冒号、实测整值到达））→ 专用错误退出 1（v0.1 无 remount/
   属主映射/安全标签语义，静默忽略会掩盖用户意图——SELinux 环境以
   `context=` 挂载是常规操作，须显式失败而非静默丢标签）；
4. 两轨皆不属的键透传 libfuse（未知 → 退出 1）——直连调用收到
   mount(8) 路径上被滤除的键（`defaults`/`auto`/`noauto`/
   `comment=`/`x-*`/`loop`/`offset=`/`sizelimit=`/传播键）时即落
   此条、退出 1：这些是 mount(8) 侧指令（fstab 自动化/回环设备/
   传播设置），直连路径无消费者，**显式声明该不对称**为设计意
   图——mount(8) 路径滤除后恒成功，直连路径报错以暴露无效意图。

`rw` 必须被接受（否则摘要首推的默认调用形态必然 exit 1）：接受为
无操作并记 stderr 警告（ro 为硬编码基线、安全不被稀释，ntfs-3g
同先例 [NTFS3G]）；`suid`/`dev` 维持报错退出 1（实测确实
到达，规范可测）。
到达的无关 VFS 键接受并忽略使 `mount -t gitfs -o
noatime,nodiratime repo dir` 这类常见习惯、fstab 的 `user`（非
root 挂载）与 boot 常见的 `nofail`/`_netdev` 均不因未知键失败。
**演进条款**：枚举名单是实测快照、对表内键不宣称穷尽（表轨兜
底）；未来 util-linux 版本使新的表外键到达 `-o` 串时（如 2.41+
的 `symfollow`），按"是否对 gitfs 无操作"人工分诊入枚举名单，并
写入维护核对单 `docs/maintenance-checklist.md`（新 util-linux 发布
→ diff man 表与 libmount 转发键集 → 分诊 → 更新枚举名单与 §5
用例），而非因枚举缺漏落入 libfuse 拒绝。

```
用法: mount.gitfs [选项] <repository> <mountpoint>
      （选项可出现在位置参数之后——mount(8) exec 助手实测 argv 为
        <repo> <dir> -f -o rw，解析须容忍 GNU getopt 式选项置换）

选项:
  -o OPT[,OPT…]       键值/开关形态。gitfs 自有键 blob-cache-size=<MiB>、
                       tree-cache-size=<MiB>（连字符与下划线拼法等价），
                       与同名长选项语义、校验完全一致；同一键重复给出
                       取"后者胜"（出现序：fstab 条目 → 命令行选项按
                       命令行先后，-o 串内从左到右），对齐 mount(8) 的
                       fstab+CLI 合并惯例，不因该覆盖流退出 1；与硬编
                       码基线同义的键（ro/nosuid/nodev/
                       default_permissions/use_ino）接受为冗余无操作；
                       `rw` 同样接受为无操作并记 stderr 警告（见上）；
                       到达 `-o` 串的无关 VFS 键按上文双轨分诊（名单
                       见上；user 族附带键实测口径见上）；`fsname=`
                       允许透传覆盖基线（cosmetic，仅影响展示名），
                       `subtype=` 覆盖基线 → 参数错误退出 1（基线保
                       护——/proc/mounts 的 type 字段与 mount -t
                       gitfs/findmnt -t gitfs 的匹配依赖它，ro/nosuid/
                       nodev/default_permissions/use_ino 同列保护）；
                       其余键原样透传 libfuse 选项解析器（如
                       kernel_cache；allow_other 需 /etc/fuse.conf 启
                       用 user_allow_other），未知键由 libfuse 拒绝 →
                       退出 1
  --blob-cache-size <MiB>  blob LRU 缓存上限（默认 64）；值为正整数
                       （十进制 MiB）——0、负数、非数字或溢出 size_t
                       → 参数错误（退出码 1）；不设人为上限，受可用
                       内存约束
  --tree-cache-size <MiB>  libgit2 对象缓存（tree/commit）总预算上限
                       （默认 256，即 libgit2 默认值）；校验规则与
                       --blob-cache-size 相同；与 blob 缓存各自独立
                       记账（见 3.5）
  --foreground         前台运行（默认守护进程化）；仅长选项形态——
                       短选项 -f 已按助手契约保留给 fake
  -f                   fake（mount(8) 的 --fake 转交形态）：完整校验
                       参数与全部选项、并同样打开仓库校验可读性（坏
                       仓库 → 退出 2——否则 mount --fake 会给 boot 时
                       才失败的 fstab 条目开绿灯），仅跳过挂载与守护
                       进程化，校验通过后退出 0
  --verbose / -v       输出路径解析与缓存命中日志，及 blob 全量解压
                       事件（每次解压一条，即解压计数：超限 open-pin
                       与缓存 miss 装载各一条，见 3.5）
  -n / -s / -N <ns>    mount(8) 转交的 no-mtab / sloppy / namespace
                       标志：容忍并忽略（-t 处理与 -s 不放宽语义见
                       上文；-r/-w 从不以标志形式转交——libmount 并
                       入 -o 串为 ro/rw，见上）
  --version / --help
```

- 挂载选项硬编码基线：`ro,fsname=gitfs,default_permissions,
  subtype=gitfs,nosuid,nodev,use_ino`（`use_ino` 令内核采用 gitfs
  填充的路径派生 `st_ino`，见 3.2）。
- 卸载：`umount <mountpoint>`（或 `fusermount3 -u`）；守护进程收到
  `SIGINT`/`SIGTERM` 亦优雅退出（见 3.4）。
- 退出码：0 = 挂载成功（守护进程化后父进程退出）或前台模式优雅
  终止或 `-f` fake 校验通过；1 = 参数错误；2 = 仓库不可读/不是
  git 仓库；3 = 挂载失败。（mount(8) 助手形态下 mount(8) 传播的
  exit 0 语义即"挂载成功"，卸载由 umount(8)/fusermount3 完成、不
  经本程序。）

## 4. 安全考虑

威胁模型概览：gitfs 为只读、内容寻址的数据面——不访问网络、无
写路径；安全风险集中于（a）不受信仓库内容（symlink 目标、绕过
fsck 的病态对象、超长名）与（b）挂载期间的仓库外部操作（外部
gc/prune、手改 refs）。逐项对策：

- `default_permissions` 让内核执行常规属主/权限检查，不依赖我们的
  实现。
- `ro + nosuid + nodev`：内容寻址的只读数据面，杜绝 setuid 与设备
  节点。
- symlink blob 内容由仓库控制，可能指向绝对路径或快照外目标，语
  义与 `git checkout`/`tar -x` 一致，不做改写；用户不应以根/特权
  身份浏览不受信仓库的挂载点。`..` 逃逸由 VFS 挡在挂载点内。
- 仓库路径经 `realpath` 规范化为绝对路径（含符号链接解析）后打开，
  避免相对路径与 TOCTOU 问题。

## 5. 工程结构（开源最佳实践）

```
gitfs/
├── CMakeLists.txt            # >= 3.16，C++17，-Wall -Wextra -Wpedantic
│                             # -Werror(CI)；install: $(sbindir)/mount.gitfs
│                             # + $(mandir)/man8
├── LICENSE                   # GPL-3.0-or-later（SPDX 标注同左）
├── README.md                 # 快速开始、语义声明（/commits 首次 open 的
│                             # 停顿语义 3.4；超限 blob open 等待语义与
│                             # 锁外解压不阻塞他请求 3.5；可缓存 LRU
│                             # 装载瞬时重复解压可能 3.5；挂载期间禁
│                             # gc 3.1；st_ino 注册表内存上界与不回收
│                             # 3.2）、FAQ
├── CONTRIBUTING.md / CODE_OF_CONDUCT.md / SECURITY.md / CHANGELOG.md
├── .clang-format             # Google 风格
├── .gitignore / .gitattributes
├── .github/workflows/ci.yml  # lint + build + test 矩阵（gcc/clang × ubuntu）
├── rfc/                      # 本目录：设计文档先行
├── src/
│   ├── main.cpp              # CLI/mount(8) 助手参数解析、fuse 启动
│   ├── gitfs.hpp/.cpp        # fuse_ops 实现（路径解析、VFS 语义）
│   ├── gitrepo.hpp/.cpp      # libgit2 RAII 封装（ref 解析、tree 下行）
│   ├── object_cache.hpp      # blob LRU（单测覆盖）
│   ├── path_map.hpp/.cpp     # "/branch/x/y" → 解析状态机（纯函数，重点单测）
│   ├── errmap.cpp            # git_to_errno
│   └── log.hpp
├── tests/
│   ├── unit/                 # Catch2 v3 [CATCH2]（FetchContent）：path_map 状态机
│   │                         # （畸形路径、Unicode、超长 oid）、LRU 逐出、
│   │                         # 错误映射表、st_ino 碰撞消歧（哈希可注入）
│   ├── integration/          # 真实挂载：fixture 仓库 + 断言（见下）
│   └── fixtures/make_repo.sh # 生成测试仓库（fixture 清单见下）
└── docs/
    ├── filesystem-semantics.md    # 对用户承诺的语义（本文 4.x 的稳定
    │                              # 化版本；必含"gc/prune 并发""空仓
    │                              # 库""readdir 字节字典序"三节与
    │                              # st_ino 注册表内存上界声明）
    ├── maintenance-checklist.md   # 维护核对单：3.7 演进条款的载体
    │                              # （新 util-linux 发布 → diff mount(8)
    │                              # "Filesystem-independent mount
    │                              # options" 表与 libmount 转发键集 →
    │                              # 按"是否对 gitfs 无操作"分诊 → 更新
    │                              # 枚举名单与本文用例）
    └── mount.gitfs.8              # man 手册（roff；install 到 man8）
```

- **提交规范**：Conventional Commits [CONVCOMMITS]
  （`feat:`/`fix:`/`docs:`…），CI 校验。**版本**：SemVer [SEMVER]；
  打 tag 出 release，CI 产出各发行版二进制 + SBOM。

**fixture 仓库（make_repo.sh）清单**——每项对应下述断言：

- 常规形态：子模块、可执行文件、中文文件名、非 UTF-8 文件名（原始
  字节透传）、嵌套分支名（`feature/x`）、annotated tag 与指向
  blob 的轻量 tag、含嵌入 NUL 的 symlink、超 PATH_MAX 无 NUL 与首
  NUL 位于 PATH_MAX 之后（如 5000 字节、首 NUL 在 4500）两类手工
  symlink blob——后两者 readlink 报 `ENAMETOOLONG` 而 st_size 分别
  报全长与截断长度（例外谓词两形态均可分辨）。
- **大 blob 置于仅含普通 blob 的专用目录**——不含 symlink 与
  `.gitfs-submodule` 合成项，即满足零解压断言的目录限定语，断言以
  它为遍历对象。
- 用 mktree/hash-object -w 手工构造含 `.` 与 `..` entry 的非法
  tree（绕过 fsck），供 readdir 跳过断言。
- detached HEAD 独有 commit（commits 清单断言）；
  `refs/remotes/origin/HEAD`（隐藏断言）；update-ref 直写的非符号
  `refs/remotes/upstream/HEAD`（按普通跟踪 ref 呈现断言）；
  update-ref 直写的裸命名空间 ref `refs/remotes/legacy`（空命名
  空间目录与 commits 清单纳入断言）。
- 手改 packed-refs 同时含 `refs/tags/foo` 与子层 `refs/tags/foo/ab`、
  `refs/tags/foo/bar`：ref foo 的 tree 构造为恰含 blob entry `bar`
  （与子 ref 同名）、排序先于 `bar` 的 blob entry `aa` 与 blob
  entry `zz`（`aa`/`zz` 均不与任何子层 ref 同名——两类来源在排序
  上互不嵌套，供合并节点并集、混排顺序 [aa,ab,bar,zz] 与任一分组
  序可分辨、`bar` 同名折叠为目录且原 blob 形态类型可分辨、最长
  ref 名匹配回退等断言）；另手改 packed-refs 同时含 update-ref
  指向 blob 的 `refs/tags/baz` 与子层 `refs/tags/baz/qux`，构成非
  commit 目标合并节点（形状失配断言）。
- `refs/replace/<oid>`（replace 不生效断言）；`refs/notes/keep` 与
  `refs/stash`（commits 入集口径断言，对齐 rev-list --all）；
  update-ref 指向 blob 的分支 ref（非 commit 目标入口 ENOENT 断
  言）。

**集成断言要点**（oracle 一律 `git --no-replace-objects ls-tree`/
`git cat-file`——与 gitfs 同为 replace 不生效语义）：

- 内容一致性；"挂载后新增 tag 立即可见"。
- 边缘：`.`/`..` entry 在 readdir/getattr 被跳过且记警告；
  `origin/HEAD` 在 `/remote` 不可见（访问 ENOENT）、直写非符号
  `upstream/HEAD` 可见；`legacy` 渲染空合成命名空间目录、ref 本体
  经 `/remote` 不可达而其 commit 入 `commits`；手改 packed-refs
  破坏 D/F 时 `/tag/foo/bar` 解析为子层 ref、警告命中（最长匹配）。
- readdir 顺序：根与 branch/tag/remote/tree 按分量原始字节字典序，
  含分组前缀与合并节点并集（tree 条目与子 ref 名混排同序；fixture
  并集为 `aa`/`ab`/`bar`/`zz`，混排序与任一分组序可分辨）；oracle
  以 `LC_ALL=C sort` 归一比对。
- symlink：嵌入 NUL 截断至首个 NUL 且 stat `st_size` 与 readlink
  返回长度一致（截断口径）；两类病理形态 readlink 均
  `ENAMETOOLONG` 而 st_size 分别报全长与截断长度。
- commits：detached HEAD 独有 commit 在列；存在 blob tag 时清单仍
  成功生成且含全部 commit oid（非 commit ref 跳过不计错）；排序
  归一后与 `git --no-replace-objects rev-list --all` 输出一致
  （notes/stash 的 commit 在列）。
- 非 commit 入口：指向 blob 的分支 ref → `/branch/<name>` ENOENT
  （与 `/tag` 同口径）；`/tag/baz` 该节点 ENOENT 而父目录折叠目录
  项仍在（以查找口径为准）；折叠歧义（`/tag/foo` 并集内 `bar`
  折叠）与形状失配（`/tag/baz` ENOENT）的警告日志各自命中。
- 元数据：分组/命名空间目录（`/branch/feature`、`/remote/origin`）
  mode 0755、nlink=2、时间戳 = 挂载时刻（非 committer time）；根、
  分组、合并节点与 tree 目录 `st_size`=4096；非 UTF-8 文件名按原
  始字节读回；`du`（512B 块口径）对普通与合成文件报块数 =
  ceil(st_size/512)。
- 超限 blob 直读：`--blob-cache-size=1` 挂载下读取大于容量的 blob，
  另以恰等于容量上限（1 MiB = 1048576 字节）的 blob 断言同走绕过
  路径（边界 `>=`）；顺序分片整读、内容逐块与 `git cat-file` 一致
  且既有缓存条目不被逐出；open-pin 语义以 **verbose 解压计数 =
  1** 直接断言（耗时比值断言只能抓 O(n²) 回归，计数断言补足其无
  法排除多次解压的盲区）；另以"整读耗时不随分片数平方增长"的宽
  松比值断言在自管 runner 上作参考检查。
- **零解压断言**：`-v` 挂载下对大 blob 专用目录做 `ls -l`（getattr
  全遍历；断言限定**不含 symlink 与 `.gitfs-submodule` 合成项（子
  模块条目）的目录**——symlink 条目按 3.2 须读内容定位 NUL（病理
  形态须全量扫描）、`.gitfs-submodule` 条目的内容与 `st_size` 须
  读 commit 树根 `.gitmodules` blob 且新挂载下缓存 miss 装载解压
  同记一条日志，含任一者的目录合法触发解压、不在断言范围），verbose
  日志不出现任何解压事件（普通 blob 尺寸经 header-only 读取，与
  "getattr 不触发 revwalk"断言同构）。
- 锁外解压可选参考断言（自管 runner、宽松阈值）：超限 blob open
  解压期间并发的根 readdir 不被长时间阻塞；大容量
  `--blob-cache-size` 挂载下首次读大可缓存 blob 期间同。
- **mount(8) exec 路径九场景**（经真实 `mount -t gitfs`，需
  util-linux ≥2.35 与特权环境，CI 无特权时 skip 标记）：(1) 默认
  调用（不带 -o ro）——libmount 预置的 `-o rw` 到达助手，断言被接
  受（stderr 记警告）且挂载成功、卸载干净；(2) `mount --fake`——
  转交的 `-f` 走 fake 语义：参数、选项与仓库可读性完整校验通过
  （另以坏仓库断言 `-f` 退出 2）、mountpoint 未被挂载、退出 0；
  (3) fstab `blob-cache-size=128` + CLI `-o blob-cache-size=256`
  合并调用——自有键后者胜（挂载后读 `.gitfs.json` 的
  `cache.blob_bytes` = 256×1024²）；(4) `-o noatime`——到达被接受
  并忽略（verbose 记一条）且挂载成功；(5) fstab 含 `user` 的条目
  以非 root 用户 `mount <dir>` 触发——`user` 及其隐式附带的
  noexec,nosuid,nodev 到达后均被接受且挂载成功（对 owner/group 不
  写 noexec 存在性断言）；(6) `-o noatime,nodiratime`——经典组
  合，两条均原样到达、按键名匹配被接受并忽略（verbose 记两条）；
  (7) `-o user=alice` 键值形态——带值到达、按"截首个 = 取键名"匹
  配入忽略名单（不附带任何隐式键）；(8) `-o symfollow`——
  util-linux 2.41+ 演进键（man 表外、枚举轨收录），原样到达且被
  接受并忽略；(9) `-o nouser`（man 表内无操作键）——原样到达且被
  接受并忽略。另以直连调用断言 `-o remount`、`-o uid=1000` 与
  `-o context=system_u:object_r:user_home_t:s0`（值含冒号、整值到
  达）退出 1 且错误文案专用。
- CI 上 `/dev/fuse` 不可用时集成测试自动 skip（标记），自管 runner
  跑全量。可观测性：`-v/--verbose` 输出解析日志；错误信息含 oid
  与 errno 上下文。

## 6. 备选方案

| 方案 | 结论 |
|---|---|
| 调用 `git` 子进程获取对象 | 否决：每次 fork+协议解析开销大、错误处理脆弱 |
| fuse2 | 否决：已进维护期，fuse3 是当前主线（本机 3.18 可用） |
| 自研 packfile 解析 | v0.1 否决：重复造轮子；保留为远期性能优化项 |
| 复用 presslabs/gitfs（Python）[PRESSLABS] | 否决：语义不同（其为读写+云后端），且非本项目语言目标 |

## 7. 里程碑

1. **M1**：`/branch/<name>` 只读浏览 + 单测——端到端可演示。
2. **M2**：`/tag`、`/commit`、`/remote`、`/HEAD`、`commits` 清单、
   错误映射、statfs、`.gitfs.json`。
3. **M3**：blob LRU、性能基准、集成测试矩阵、CI 完整化。
4. **M4**：文档定稿、0.1.0 发布（首个 SemVer tag）。

## 8. 参考文献

### 8.1 规范性引用

- [BCP14] Bradner, S., "Key words for use in RFCs to Indicate
  Requirement Levels", BCP 14, RFC 2119; Leiba, B., "Ambiguity of
  Uppercase vs Lowercase in RFC 2119 Key Words", BCP 14, RFC 8174.
- [POSIX] IEEE Std 1003.1-2017（POSIX.1-2017），The Open Group
  Base Specifications Issue 7：PATH_MAX/NAME_MAX 与 errno 常量
  （ENOENT/EIO/EROFS/ENAMETOOLONG/ENOTSUP）语义。
- [MANPAGES] Linux man-pages：mount(8)（EXTERNAL HELPERS 契约与
  "Filesystem-independent mount options" 表）、symlink(7)（内核
  接受的目标长度上限 PATH_MAX−1）。
- [GIT] Git Documentation：git-check-ref-format(1)、git-rev-list(1)、
  git-update-ref(1)、git-hash-object(1)、gitrepository-layout(5)。
  https://git-scm.com/doc
- [LIBGIT2] libgit2 API 文档与源码：线程安全保证、
  GIT_OPT_SET_CACHE_OBJECT_LIMIT / GIT_OPT_SET_CACHE_MAX_SIZE、
  git_odb_read_header。https://libgit2.com
- [FUSE3] libfuse 3.x 文档与源码：attr/entry timeout 缺省值、xattr
  缺省 ENOSYS、fi->fh 约定。https://github.com/libfuse/libfuse
- [UTILLINUX] util-linux 2.42.3 源码（libmount）：exec_helper、
  optlist 的转交与滤除行为。
  https://github.com/util-linux/util-linux

### 8.2 资料性引用

- [NTFS3G] ntfs-3g——mount(8) 对助手预置 rw 时接受为无操作的
  先例。https://github.com/tuxera/ntfs-3g
- [PRESSLABS] presslabs/gitfs——读写 + 云后端的同类项目（§6 的
  比较对象）。https://github.com/presslabs/gitfs
- [SEMVER] Semantic Versioning 2.0.0. https://semver.org
- [CONVCOMMITS] Conventional Commits 1.0.0.
  https://www.conventionalcommits.org
- [CATCH2] Catch2 v3 测试框架. https://github.com/catchorg/Catch2

## 附录 A. 评审决议索引（资料性）

2026-09-24 评审（决议 Q1–Q7）及其后十九轮 review（Q8–Q26 评审与
勘误、Q27–Q39 定稿后复核钉缝）的全部终局口径均已并入 §3/§5 正文；
本表为可追溯索引（决议 → 终局口径 → 正文锚点）。逐轮勘误链、round
编号与提交拆分等过程记录见 git 提交历史（10f0dce…db3a576）；本文
压缩定稿不改变任何已决语义。

| 决议 | 终局口径（锚点） |
|---|---|
| Q1/Q2 | `/commit/<oid>` 仅完整 oid、不支持短前缀；`/commit` readdir 恒空，检索走 `commits` 清单（3.1） |
| Q3 | 子模块渲染为空目录 + `.gitfs-submodule` 说明文件（3.2） |
| Q4 | v0.1 即含 `/remote/<remote>/<branch>` 与 `/HEAD`（3.1） |
| Q5 | GPL-3.0-or-later（§5） |
| Q6 | clang-format Google 风格（§5） |
| Q7 | 单互斥串行化 libgit2；revwalk 分 chunk 让锁；M3 基准不达标再引入按 oid 分片锁（3.4） |
| Q8 | `refs/replace` 不遵循、透传原始对象（3.1） |
| Q9 | `st_atime`≡`st_mtime`；`.gitfs-submodule` mode 0644、mtime=committer time、内容 url=/commit= 两行；unborn HEAD 时 `head` 为 null（3.2/3.6） |
| Q10 | statfs `f_blocks`=本地 ODB 占用；`-o` 键处置经多轮演进为 3.7 双轨分诊（现行拒绝名单 `suid/dev`）（3.3/3.7） |
| Q11 | libgit2 per-type 上限：tree 抬 1MiB、blob 写死 0；总预算即 `--tree-cache-size`（3.5） |
| Q12 | gc/prune 瞬态语义与空仓库行为显式声明；缓存参数校验（正整数、非法 → 退出 1）（3.1/3.7） |
| Q13 | mount(8) 助手形态三调用等价；`--blob-cache-size`/`--tree-cache-size`；`-o` 键值形态与覆盖序（经 Q21c 定为后者胜）（3.7） |
| Q14 | `st_ino` 路径哈希派生；commits 拓扑序确定性；生成失败 EIO 不缓存；xattr → ENOTSUP；kernel_cache 仅数据页（3.2/3.5/3.3） |
| Q15 | `fsname=` 可覆盖、`subtype=` 基线保护；commits 生成前 mtime=挂载时刻；st_ino 碰撞注册表消歧（3.2/3.7） |
| Q16 | commits 入集=全部 refs ∪ HEAD（peel 失败跳过）；注册表内存上界声明；readdir 字节字典序全域钉住；合成元数据补全；超限 blob 绕过缓存（3.1/3.2/3.5） |
| Q17 | 超限 blob open-pin 一次性解压；非 commit 入口统一 ENOENT + 警告（3.1/3.5） |
| Q18 | open-pin 解压移至锁外；每次全量解压记一条 verbose（解压计数）（3.4/3.5） |
| Q19 | LRU 装载同构锁分段（瞬时重复解压可能、不承诺 single-flight）；blob 尺寸一律 header-only（3.4/3.5/3.2） |
| Q20 | `-v` 帮助文本涵盖超限 open-pin 与缓存 miss 装载两类解压事件（3.7） |
| Q21 | `rw` 接受记警告（libmount 无条件预置）；`-f` 即 fake、前台仅 `--foreground`；自有键后者胜；st_blocks/blksize/rdev 钉住（3.2/3.7） |
| Q22 | VFS 键到达口径勘误（滤除仅固定子集、其余原样到达）；`-N` 容忍忽略；选项后置解析；退出码 0 措辞=挂载成功（3.7） |
| Q23 | 名单补全并钉"截首个 = 取键名"；`context=` 族专用错误；`-s` 不放松键拒绝；`-f` 含仓库校验（坏仓库 → 2）；user 族附带键实测口径（3.7） |
| Q24 | 分诊改双轨锚点（枚举优先 + 表轨兜底）并加演进条款；`-o rw,ro` 折叠钉住（3.7） |
| Q25 | `acl`/`quiet`/`showexec`/`bsdgroups` 为表外转发键、改入枚举轨，表轨限定仅认表内所列；维护核对单落点 `docs/maintenance-checklist.md`；直连滤除键不对称声明；带值 ro/rw 形态注记（3.7/§5） |
| Q26 | 助手契约 `-t` 子句（`-t gitfs` 接受、其余退出 1）；分组前缀节点元数据=挂载时刻；仅含隐藏 HEAD 的命名空间渲染空目录；目录 `st_size`=4096 与超限边界 `>=`（3.7/3.2/3.1/3.5） |
| Q27 | symlink `st_size`=截断长度（与 readlink 对齐）；非符号远端 HEAD 按普通跟踪 ref；packed-refs 破坏 D/F 取最长 ref 名匹配回退 + 折叠警告（3.2/3.1） |
| Q28 | 合并节点 readdir=tree 条目 ∪ 子 ref 名并集、同名折叠、形状失配以查找口径为准；截断后长度 ≥ PATH_MAX 的手工 symlink blob 为不变式显式例外（3.1/3.2） |
| Q29 | 并集输出同按分量原始字节字典序、混排不分组；裸命名空间 ref 本体不可达为声明性行为、commits 仍纳入（3.1/3.5） |
| Q30 | 裸 ref 不可达声明与合并节点口径的交互限定（仅裸形态成立）；readdir 表格行自包含点名合并节点；symlink 代价论证只锚定正常写入（3.1/3.3/3.2） |
| Q31 | 目录 `st_size`=4096 行内点名合并节点（经完整 ref 节点归 tree 口径）；man 头注释随勘误升版为流程惯例（3.2） |
| Q32 | fixture 增非 commit 目标合并节点（`baz` 指向 blob + `baz/qux`），形状失配断言可执行（§5） |
| Q33 | fixture 并集增 `zz`、钉 `bar` 为 blob——并集构成与折叠类型两形态可分辨；折叠歧义与形状失配两类警告补断言（§5） |
| Q34 | fixture 增 `aa` 与 `foo/ab`——混排序 [aa,ab,bar,zz] 与任一分组序可分辨（§5） |
| Q35 | 3.3 getattr 行主语改"普通 blob"并内联 symlink 例外；零解压断言限定不含 symlink 的目录；例外谓词拓宽为"截断后长度 ≥ PATH_MAX（无论是否含 NUL）"、ENAMETOOLONG 边界钉 `>=`；多分量子 ref 按首分量入列；折叠措辞点名两场景避免与分组前缀节点撞名（3.3/3.2/§5/3.1） |
| Q36 | 零解压断言限定语补排除 `.gitfs-submodule` 合成项（双排除封闭）；man File metadata 措辞与例外形态对齐（§5） |
| Q37 | `.gitmodules` 读取按普通 blob 走 LRU（LRU 即备忘层；病态超限走绕过分支逐次解压各记日志）；fixture 钉大 blob 专用目录满足断言限定（3.2/§5） |
| Q38 | fixture 注释自引章节号改"本文"（纯措辞，§5） |
| Q39 | 同款自引第二实例与计数口径措辞收尾（纯措辞，过程记录见 git 历史） |
