# RFC 0000 — gitfs：将 git 仓库挂载为只读 FUSE 文件系统

- RFC 编号: 0000
- 标题: gitfs — read-only git-to-FUSE 文件系统
- 状态： Accepted（2026-09-24 评审通过，决议见 §7；2026-09-24 补充决议见 §7.1、§7.2、§7.3、§7.4、§7.5、§7.6、§7.7、§7.8、§7.9、§7.10、§7.11、§7.12、§7.13、§7.14、§7.15、§7.16（§7.11–§7.15 为定稿后 mount(8) 助手协议核验勘误，§7.16 为定稿后收尾勘误））
- 日期: 2026-09-24
- 目标版本: 0.1.0

## 0. 摘要

实现一个 C++17 编写的命令行程序 `mount.gitfs`（安装于 `$(sbindir)`，遵循
mount(8) 助手命名约定 `mount.<type>`），把一个本地 bare 或普通 git 仓库挂载为
**只读** FUSE 文件系统。支持三种等价调用：`mount -t gitfs <repo> <dir>`、
`/etc/fstab` 条目、直连 `mount.gitfs <repo> <dir>`（见 3.7）。挂载点根目录
暴露下列入口：

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
7. 以 mount(8) 助手形态（`mount.gitfs`）与系统集成：`mount -t gitfs` 与
   `/etc/fstab` 条目可直接使用（Q13）。

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
                           仅是默认分支树的重复，符号语义也无法在目录树中
                           表达；命名空间存在性以 refdb 为准——仅含该
                           隐藏 HEAD 的命名空间仍渲染为**空目录**、
                           不抑制（Q26c），跟踪 ref 出现后由实时枚举
                           自动填充
/HEAD                    → 当前 HEAD commit 的 root tree（unborn → ENOENT）
/commit/<full-oid>       → 该 commit 的 root tree；oid 长度随仓库对象格式
                           （sha1=40、sha256=64），仅接受**小写**十六进制——
                           大写 hex 与其他非法字符同判 → ENOENT（Q16f，与
                           commits 清单的小写输出一致）；
                           oid 须指向 commit 对象本身，不做 peel——
                           annotated tag/tree/blob 的 oid → ENOENT
/commits                 → 只读文件：全部可达 commit（可达自 refs
                           与 HEAD；入集与 peel 口径见 3.5——非 commit
                           目标的 ref 跳过、不算错误）的完整 oid，每行一条，
                           拓扑序（确定性实现钉住在 3.5：
                           GIT_SORT_TOPOLOGICAL + 排序入队）；不支持
                           短前缀解析，用户
                           自行 `grep ^<前缀> commits` 检索
```

- `/commit` 的 readdir **返回空列表**（不可枚举）：巨型仓库（linux 内核
  ~110 万 commit）在 readdir 的 offset 续读语义下要么缓存 ~100MB 条目、
  要么 O(n²) 重走 revwalk，且首次全量遍历耗时数秒会挂起列目录进程；
  oid 检索需求改由 `commits` 清单文件承担（首次 `open` 时生成、缓存，
  整块缓冲、按 offset 切片读，见 3.5）。

- ref 名规则（git check-ref-format）禁用空格、控制字符及 `~^:?*[\` 等，
  天然是合法文件名；可含 Unicode（如中文），直接作为文件名。
- **ref 目标非 commit 的统一口径（Q17b）**：`/tag/<name>`、
  `/branch/<name>`、`/remote/<remote>/<b>` 与 `/HEAD` 的解析一律
  **peel 至 commit** 后取其 root tree；peel 失败——ref 直接指向
  blob/tree（除 tag 外，分支、远端跟踪 ref 与 HEAD 经手工
  `git update-ref` 同样可置于该形态，git 均允许）——一律 `ENOENT`
  并记警告日志，v0.1 不呈现非 commit 目标（与上文 `/tag` 条目同
  口径，钉住全域行为、避免实现者按入口类推不一；§3.5 的 `commits`
  清单生成对同类 ref 则跳过、不算错误——入口不可见、清单不受
  拖累，两处口径互补）。
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
  目录，getattr 语义一致——完整 ref 节点即其 root tree（元数据按
  tree 目录口径，mtime = 所属 commit 的 committer time，见 3.2）；
  **分组前缀节点**（`/branch/feature`、`/tag/v1.0`、`/remote/origin`
  等命名空间目录）为合成节点，钉为 `S_IFDIR|0755`、`nlink=2`、
  `st_mtime/ctime/atime` = 挂载时刻——与五个入口目录同口径（见
  3.2/Q16d），非 tree 目录、不取 committer time（Q26b）。
- **readdir 顺序全域钉住（Q16c）**：根目录、`branch/tag/remote`（含嵌套
  ref 的分组前缀目录）与 tree 目录一律按**路径分量原始字节（memcmp，无
  locale 参与）字典序**输出。tree 目录**重排**为字节字典序而非沿用
  git tree 的内在条目序——后者按"目录名附加 `/` 后参与比较"的规则，
  在 `foo`（目录）与 `foo.txt`（文件）这类组合上与纯字节序相反
  （`0x2E '.'` < `0x2F '/'`，git 序把 `foo.txt` 排在 `foo/` 之前）——
  换取整个挂载统一、可复现、不依赖 git 内部排序实现的枚举顺序；
  `.gitfs-submodule` 合成项按其文件名参与同一排序；`.`/`..` 不由
  gitfs 返回（由内核自产）。集成测试直接断言字节序，或以
  `LC_ALL=C sort` 归一后与 `git ls-tree` 比对集合（见 §4）。
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
  恢复；gitfs 不承诺挂载期内对象集不变。`commits` 清单生成期间命中
  同一口径：revwalk 中途失败的那次 `open` 返回 `EIO`、半成品不缓存、
  下次 `open` 重试（见 3.5）。运维建议：挂载期间禁用自动
  gc（`git config gc.auto 0`）或接受上述瞬态错误（README 与
  filesystem-semantics.md 中声明，见 §4）。

### 3.2 权限与元数据映射

| git entry mode | st_mode | 说明 |
|---|---|---|
| `040000` | `S_IFDIR \| 0755` | 目录 |
| `100644` | `S_IFREG \| 0644` | 普通文件 |
| `100755` | `S_IFREG \| 0755` | 可执行 |
| `120000` | `S_IFLNK \| 0777` | 符号链接，`readlink` 返回 blob 内容 |
| `160000` | `S_IFDIR \| 0755`（空目录）+ 说明文件 | 子模块：渲染为空目录，旁边放 `<name>.gitfs-submodule` 文本文件（`<name>` 即该子模块目录名，如 `deps/libfoo` → `deps/libfoo.gitfs-submodule`；内容含 url 与 commit oid）；若树中已存在与该合成文件名相同的真实条目，真实条目优先、省略合成文件（记警告日志） |

- `st_size`：blob 的原始字节数（libgit2 直读，不做过滤）。**尺寸获取
  路径钉住（Q19b）**：blob 尺寸一律经 **header-only 读取**
  （`git_odb_read_header`）获得，不触发 blob 全量解压——`getattr` 与
  3.5 的超限准入判定同用此路径，含多 GB blob 目录的 `ls -l`（getattr）
  或 open 阶段的超限判定都不为取 size 付整块解压的代价，与 3.3 已
  钉住的"getattr 不触发 revwalk"同构补全（见 3.3、3.5）。
- `st_mtime`/`st_ctime`：所属 commit 的 committer time；同一快照内全部一致，
  便于 rsync 类工具判断。
- `st_atime`：与 `st_mtime` 同值（committer time），不随读取更新——只读
  快照语义，避免逐次 stat 漂移干扰 rsync 类工具；主流发行版 `relatime`
  挂载下内核本就不强制刷新 atime，用户无感知。
- `st_nlink`：目录为 2，文件为 1（简化，不做精确统计）。
- `st_blocks`/`st_blksize`/`st_rdev`（Q21d）：libfuse 不按 `st_size`
  自动推导 `st_blocks`，缺省 0 会让 `du` 对全部文件报 0 块——显式钉为
  `st_blocks = ceil(st_size / 512)`（`commits`/`.gitfs.json`/
  `.gitfs-submodule` 等合成文件同口径，按其内容字节数计），
  `st_blksize = 4096`（与 statfs 汇报的块大小一致），
  `st_rdev = 0`（任何 entry 均非设备节点，与 nosuid/nodev 基线同向）。
- `st_ino`：由完整 VFS 路径字节的 64 位稳定哈希派生（`branch/`、
  `commits`、`.gitfs.json` 等合成入口同口径，根固定为 FUSE_ROOT_ID=1），
  跨重挂载确定，`tar`/`find -inum`/`diff` 等工具得到可复现的 inode。
  **刻意不做 oid 派生**——同一 blob 出现在多个 ref 路径下若共享 inode，
  会被 `tar -c`/`rsync -H` 误判为硬链接；路径派生 + 碰撞消歧（见下）
  保证不同路径必得不同 inode，且 inode 相等**不**承诺内容同一（本
  文件系统无硬链接语义）。
  **哈希碰撞消歧（Q15c）**：哈希函数可注入（单测可强制构造碰撞）；
  挂载期维护 `primary_hash → 首个占用路径` 注册表，受 3.4 单互斥保护；
  后到路径命中已占用哈希时，依次取 `H(path || '#' || k)`（k=1,2,…）
  中最小的未占用值。备选值本身由路径确定，但占用归属取决于遭遇顺序
  ——跨重挂载可复现性**在冲突路径上除外**（10⁶ 条目生日碰撞概率
  ≈ 3×10⁻⁸，实际不可达；注册表是正确性兜底而非预期路径）。
  挂载基线因此增补 `use_ino`（见 3.7），由 gitfs 填充该派生值。
  **注册表内存上界（Q16b，显式声明）**：条目**懒注册**——仅
  getattr/readdir 实际解析过的路径才入表，不做全仓库预注册；占用上界
  ∝ 挂载期内被触及的**不同** VFS 路径数（每条 ≈ 路径字节数 + 数十
  字节哈希桶/指针开销，百万级触及路径 ≈ 数十 MB 量级）。条目在挂载期
  内**不回收**（分支删除后仍保留）：inode 号永不复用，规避"同号异
  对象"对 `find -inum`、NFS 句柄类工具的误导；该上界与不回收语义在
  README 与 filesystem-semantics.md 中声明（见 §4）。
- `st_uid`/`st_gid`：挂载进程的 uid/gid（fuse 默认行为）。
- 合成文件的 `st_mtime`：`commits` 为其生成时刻——**生成前**（§3.3 的
  `st_size=0` 窗口期）为**挂载时刻**（Q15b，与 `.gitfs.json` 同口径：
  占位符属于这次挂载、内容属于那次生成；不逐次取当前时钟，避免逐次
  stat 漂移），首次 open 后跳变为真实生成时刻（与 size 0→真实值同构）；
  refs 指纹变化触发重建后，stat 报**当前缓存版本**的生成时刻——正在
  钉住旧版本读的句柄可能看到比自己快照更新的 mtime，与 3.5"下一次
  open 起可见新版本"口径一致。`.gitfs-submodule` 为所属 commit 的
  committer time（内容确定性派生自树，避免逐次 stat 漂移）；
  `.gitfs.json` 为挂载时刻（快照语义，见 3.6）。
- **合成入口的其余元数据（Q16d；Q26b/d 扩展）**：`commits` 与
  `.gitfs.json` 为
  `S_IFREG | 0444`、`nlink=1`（写路径本就 `EROFS`，mode 与语义一致，
  `cp` 类工具按只读源处理）；根目录与 `/branch`、`/tag`、`/remote`、
  `/commit`、`/HEAD` 五个入口目录的 `st_mtime/ctime/atime` 为**挂载
  时刻**（`nlink=2` 按通用规则）——入口集合虽实时枚举，时间戳钉住
  挂载快照不漂移，与 `.gitfs.json` 同口径；**合成分组/命名空间目录**
  （嵌套 ref 的前缀节点：`/branch/feature`、`/tag/v1.0`、
  `/remote/origin` 等，见 3.1）同口径钉为 `S_IFDIR|0755`、`nlink=2`、
  `st_mtime/ctime/atime` = 挂载时刻——非 tree 目录、不取 committer
  time（Q26b）；**目录 `st_size` 恒为 4096**（根、入口、分组与 tree
  目录统一，Q26d——与 `st_blksize` 同量级的目录尺寸惯例值，根 `/`
  不再留未定口径）。
- `.gitfs-submodule` 说明文件：mode `0644`，内容为两行 `key=value` 文本——
  `url=<submodule url>` 与 `commit=<完整 oid>`（各以 LF 结尾）；url 取自该
  commit 树根 `.gitmodules` 中对应 path 的条目，缺失或无对应条目时 `url=`
  置空并记警告日志。

### 3.3 FUSE 操作实现

| 操作 | 行为 |
|---|---|
| `getattr` | 路径 → 对象（3.1），失败 `ENOENT`；`commits` 恒为纯缓存读（未生成时 `st_size=0`），**不**触发 revwalk 或指纹重算（见 3.5）；blob 的 `st_size` 经 header-only 读取（`git_odb_read_header`）获得，**不**触发 blob 全量解压（见 3.2/Q19b，与"不触发 revwalk"同构） |
| `readdir` | 根：固定列表；`branch/tag/remote`：枚举 ref（含 `/` 的名字按目录分组）；`commit`：**恒为空**；tree：枚举 entries（含 `.gitfs-submodule` 合成项）——全部目录（含根）的输出顺序一律按分量原始字节字典序（见 3.1）。注册 `opendir/releasedir`：枚举列表快照挂于 `fi->fh`，保证单目录流内 offset 续读稳定（fuse3 要求），跨目录流实时反映 ref 变化 |
| `open`/`release` | 仅校验 `O_RDONLY` 系标志（写标志 → `EROFS`，与内核对 ro 挂载的判定一致）；`commits` 首次 `open` 触发生成（见 3.5），且把当前缓冲版本**钉住于 `fi->fh`**——同一次 open 的所有 read 分片读自同一快照，refs 中途变化不影响（与 readdir 的目录流快照同构），`release` 时解除钉住；超限 blob（≥ `--blob-cache-size`，边界钉住见 3.5/Q26d）的 `open` 同构 open-pin：一次性 lookup（单互斥内）+ 全量解压（锁外执行，不阻塞全挂载其他请求），解压块钉住于 `fi->fh`、`release` 释放（见 3.5/Q18a）；`.gitfs.json` 挂载期内不可变，无需钉住 |
| `read` | 定位 blob（可缓存者经 LRU 缓存；超限者读 open 时钉住的解压块，见 3.5），拷贝 `[offset, offset+size)` 越界截断；合成文件（`commits`、`.gitfs.json`、`.gitfs-submodule`）为整块只读缓冲，`commits` 读 open 时钉住的版本 |
| `readlink` | symlink blob 内容；内容含嵌入 NUL 时**截断至首个 NUL**（内核 symlink 目标不可含 NUL；与 `git checkout` 的事实行为一致，显式同语义而非 `EIO`）；空 blob → 返回长度 0 的空目标；超过 PATH_MAX → `ENAMETOOLONG` |
| `statfs` | 汇报本地 ODB 占用为 `f_blocks`（全部 packfile 字节 + loose 对象字节；alternates 指向的外部存储不计入，启用 alternates 时 verbose 日志提示），块大小 4KiB；`f_bfree = f_bavail = 0`——只读卷惯例是 0 空闲，`df` 显示 100% 已用，向用户明确传达"无任何可写空间"（若报全量可用，`df` 会显示 0% 已用，易误导）；`f_files = f_ffree = 0`（精确 inode 计数需全量遍历，v0.1 不承诺，内核与 `df` 均容忍 0） |
| 其余（`mknod/mkdir/write/…`） | 返回 `EROFS` 或不注册（fuse3 只读挂载兜底）；**xattr 族**（`getxattr/setxattr/listxattr/removexattr`）一律不注册 → libfuse 缺省 `ENOSYS`，内核标记“无 xattr”后统一向用户态报 `ENOTSUP`（SELinux 等环境的 `security.*`/statx 附加字段查询命中此路径，干净短路而非逐次回环） |

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
- **超限 blob 全量解压同为例外（Q18a）**：open 时单互斥内仅做
  `git_blob` lookup/句柄获取，全量解压在**锁外**执行——解压块只归
  该 open 的句柄所有、无共享可变状态，与上述让锁引用同一安全性
  依据（libgit2 1.x 同 repository 并发只读）；避免数 GB blob 解压
  期间阻塞全挂载（见 3.5）。**可缓存 blob 的 LRU 装载同构分段
  （Q19a）**：锁内 lookup+双检、锁外解压、回锁复查后插入（见 3.5）
  ——`--blob-cache-size` 无人为上限，接近上限的单个可缓存 blob
  若整段持锁解压会让全挂载停摆数秒，与让锁动机残余不对称；分
  段后单互斥内不再有毫秒级以上的整块解压长持锁，本节的让锁
  动机全域对称。
- RAII 封装所有 libgit2 句柄（`git_repository`、`git_tree`、`git_blob`…），
  自定义 deleter 的 `std::unique_ptr` 别名，异常安全，热路径无异常。
- `SIGINT`/`SIGTERM` → `fuse_session_exit` 优雅卸载；同时支持 `umount` 外部卸载。

### 3.5 缓存与性能

- **blob LRU 缓存**：键 = blob oid，值 = 不可变字节串；容量按字节计，
  默认 64 MiB，`--blob-cache-size` 可调（Q13 前名 `--cache-size`）。命中则
  `read` 为纯内存拷贝。
  **超限 blob 绕过缓存、open-pin 一次性解压（Q16e，Q17a 钉住解压
  时机，Q26d 钉住边界）**：单个 blob **大于等于**（`>=`）当前
  `--blob-cache-size` 时**不插入缓存、
  也不触发既有条目逐出**（为单个超限对象清空整池只会引起缓存抖动；
  边界刻意取 `>=` 而非 `>`——恰等于容量上限的 blob 若尝试入池，
  会为容纳它逐出整池其余条目、自身又几乎占满全池，等效清空整池，
  与绕过动机直接矛盾，Q26d）。
  超限判定所需的 blob 尺寸经 3.2 的 header-only 读取获得（Q19b），
  `open` 的准入判定不因取 size 触发全量解压。
  其解压时机钉住为**与 `commits` 同构的 open-pin**（见 3.3）：
  `open` 时完成一次 `git_blob` lookup 与**全量解压**，解压后字节块
  （git_blob 句柄的 rawdata）钉住于 `fi->fh`，该 open 内所有 read
  分片直接自该块切片拷贝，`release` 时释放——单次 open 的顺序
  分片读**只解压一次**。**锁边界（Q18a）**：单互斥内仅做 lookup/
  句柄获取，**全量解压移至锁外执行**——数 GB blob 解压需数秒，
  若整段持锁则全挂载停摆，与 3.4 专设分 chunk 让锁的动机相悖；
  解压块仅归该 open 的句柄所有、无共享可变状态，让锁窗口内其他
  线程只做同 repository 的并发只读，libgit2 1.x 已保证安全（与
  3.4 让锁引用同一依据）。代价是该 open 自身的返回时间仍与全量
  解压同阶（数 GB → 数秒，调用方需预期），该等待语义与 `commits`
  首次 open 同构，README 中声明；每次全量解压记一条 verbose 日志
  （解压计数），为“只解压一次”提供直接可观测钩子（§4 断言用）。
  逐 read 重做 lookup+全量
  解压的字面直读会使大 blob 顺序分片读呈 O(size²) 解压放大，故
  不采用：blob per-type 缓存已写死 0（见下）、自家 LRU 又不收
  超限对象，每次 read 必然重新解压；且解压结果不在内核页缓存
  ——页缓存只保留 mmap 的 pack 原始字节——“页缓存兜底”并不能
  消除重复解压。**内存峰值（显式声明）**：超限 blob 的解压块不
  进共享缓存、按 open 独立持有，峰值 = Σ 各未 `release` 的超限
  blob open 所持 blob 大小（并发打开同一超限 blob 即“并发 open
  数 × blob 大小”量级），由调用方自行控制；v0.1 不做跨 open
  引用计数共享，流式按需解压（解压块不整块驻留）留作后续优化。
  open 时装载失败的错误按 3.3 映射返回（对象被外部 gc 删除 →
  `ENOENT`、pack 中途失效 → `EIO`），不产生钉住句柄。
  **可缓存装载的锁分段（Q19a，修订 Q18a 的"锁内 single-flight"
  收窄口径）**：`--blob-cache-size` 无人为上限（3.7），用户调大后
  接近上限的单个可缓存 blob 若整段在锁内装载，将在锁内解压数秒
  ——恰好重现 Q18a 专门移到锁外要避免的全挂载停摆，与 3.4 的
  让锁动机残余不对称。故可缓存 blob 的 LRU 装载与超限 open-pin
  **同构分段**：**锁内** lookup（命中即返回）并标记装载意图，
  **锁外**完成对象读取与解压（解压块归装载线程私有、无共享可变
  状态，libgit2 1.x 同 repository 并发只读安全），**回锁复查**——
  期间他线程已插入同一 oid 则丢弃自家块复用缓存项，否则插入 LRU
  （插入时容量不足则整条不入池，退化为本次调用持有私有块的
  "绕过"路径，与超限语义合流）。代价是同一 miss 的并发首载可能
  **瞬时重复解压**（败者块即刻丢弃），不承诺严格 single-flight
  ——与超限 blob 各 open 并行解压各自的块同构，瞬时内存上界 =
  Σ 各并发装载线程所持解压块大小（并发数有限且败者块即刻释放），
  远优于让全挂载为单个大可缓存 blob 停摆数秒；正常工作集下重复
  窗口为毫秒级。锁内剩余工作（lookup、LRU 逐出与记账）为微秒
  级，3.4 的单互斥不再出现毫秒以上的整块解压长持锁。缓存装载的
  解压同样记一条 verbose 日志（与超限解压计数同钩子，§4 的 getattr
  零解压断言据此观测）。
- **tree/commit 依赖 libgit2 内置对象缓存，init 时显式调参**：默认 per-type
  上限仅 4KiB（源码 `cache.c` 的 `git_cache__max_object_size[]`），序列化
  超线的大目录 tree（~100+ entry 即超）不进缓存；而高层 fuse API 每个
  syscall 自 root tree 重解析 O(depth) 次、attr/entry_timeout 又显式置 0
  （见下），tree 命中率就是 `ls -R`/`find`/`du`/rsync 类元数据负载性能的
  全部。故 init 时以 `GIT_OPT_SET_CACHE_OBJECT_LIMIT(GIT_OBJECT_TREE, 1MiB)`
  抬线，COMMIT 同抬（commit 天然 <4KiB，仅防御病态巨型 merge commit，
  无代价）；总预算 `GIT_OPT_SET_CACHE_MAX_SIZE` 设为 `--tree-cache-size`
  的值（默认 256MiB，即 1.9.7 的实测默认值，Q13 起可调），与
  `--blob-cache-size` **各自独立记账、互不挤占**，校验规则相同（见 3.7）。
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
- `/commits` 清单：**首次 `open` 时** revwalk 全量生成（**入队集合钉住
  （Q16a）**：枚举 `refs/` 下**全部** ref——heads/tags/remotes 之外，
  notes、stash、replace 等特殊命名空间一并纳入，对齐
  `git --no-replace-objects rev-list --all` 的 oracle（实测 git 对该
  集合同样全量枚举）；符号引用（如 `refs/remotes/<remote>/HEAD`）先
  解析至目标 ref 再处理；每个目标**逐个 peel 至 commit**，peel 失败
  者——轻量 tag 指向 blob/tree（§3.1 合法形态）等非 commit 目标——
  **跳过并记 verbose 日志，不算错误**（`git rev-list --all` 对此类
  ref 亦静默跳过），绝不因个别非 commit ref 使整个清单生成失败或
  退化为 `EIO`；**另加 HEAD**——detached HEAD 独有的 commit 也纳入
  清单，与 §0 "全部可达"的宣称一致，即可达集 = 全部 refs ∪ HEAD。
  replace ref 以普通 ref 身份入队（其指向的替换 commit 若在 ODB 中
  即入清单），但对象读取不遵循替换——与 §3.1/Q8 的透传语义一致），
  整块缓存；
  以 refs 指向集合**加 HEAD 指向**的指纹为失效键，任一变化才重建。
  **拓扑序确定性的实现钉住**：walker 设 `GIT_SORT_TOPOLOGICAL`，且入队
  序固定——收集到的完整 ref 名列表按字典序显式排序后逐个 push（不
  依赖 refdb 迭代器内部顺序），HEAD 恒最后入队；拓扑约束之外的并列
  commit（互不为祖先的平行链）出队次序由该入队序唯一决定，清单跨
  进程、跨重挂载逐字节可复现。
  **生成失败的 open 语义**：revwalk 中途因对象被外部 gc/prune 删除而
  失败（`git_revwalk_next` 返回错误）时，该次 `open` 返回 `EIO`，
  半成品缓冲丢弃、不写入缓存、指纹不更新（等同“从未生成过”），
  single-flight 等待方收到同一失败；下一次 `open` 从头重试，gc
  结束后即成功——与 3.1 的瞬态错误口径一致。
  revwalk 按 3.4 的分 chunk 锁策略执行，生成期间不长期阻塞其他请求。
  **生成与指纹比对只发生在 `open`**：生成前 `getattr` 报告
  `st_size = 0`，故 `ls -l`/`stat`/文件管理器枚举不触发秒级 revwalk，与
  3.1 将 `/commit` readdir 置空的同一理由保持一致；生成后 `st_size` 为
  真实值。单次 open 经 `fi->fh` 钉住缓冲版本直至 release（见 3.3），跨
  open 语义为"下一次 open 起可见新版本"；并发首次 open 的 single-flight
  串行化见 3.4。挂载后后台线程预生成留作后续
  可选优化（如 `--prewarm`），v0.1 不做。110 万 commit ≈ 45MB 文本
  （sha1 口径：每行 41B 含 LF；sha256 仓库为 65B/行 ≈ 71.5MB——
  65B × 110 万，Q16(f) 勘误原 74MB 口径），内存与
  grep 均可接受。
- FUSE 侧默认 **不开启 kernel_cache**：显式置 `attr_timeout=0、
  entry_timeout=0`（注意 fuse3 默认值均为 1s，不显式置 0 则有 1 秒陈旧窗口）：
  ref 是可变的（分支可能被删），正确性优先；提供 `-o kernel_cache` 透传给
  高级用户在“仓库挂载期间不变化”场景下自行开启。澄清：`kernel_cache`
  **只作用于数据页缓存**（read/mmap 页跨 open 复用），开启它**不**改变
  attr/entry timeout（仍恒 0，元数据每次穿透）；想要元数据陈旧窗口须
  另行显式传 `attr_timeout`/`entry_timeout`（与 `kernel_cache` 相互独立，
  一致性风险自担）。blob 不可变，内核页缓存
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
  "cache": { "blob_bytes": 67108864, "tree_bytes": 268435456 }
}
```

便于脚本探测与调试；与仓库内容无命名冲突（根目录不呈现仓库树，合成文件
均为固定名）。

**非 UTF-8 仓库路径的转义（Q16f）**：JSON 文本必须是合法 UTF-8，
`repository` 字段对仓库路径中构成**非法 UTF-8 序列**的字节按 `%XX`
百分号转义（ASCII 与合法多字节序列原样保留），任意文件系统路径都能
产出可被严格 JSON 解析器接受的文本；`head` 为 ref 名或 oid（ASCII）、
`mounted_at`/`cache` 数值不受影响。该转义**仅保证 JSON 合法、
不承诺可逆**——路径中本就存在的字面 `%XX`（ASCII 百分号 + 两位
十六进制）与转义产物在输出中不可区分，探测脚本不得据此逆推原始
路径字节（需精确路径请用挂载参数或系统侧信息）。

**失效语义（显式声明）**：`.gitfs.json` 是**挂载时刻的快照**，挂载期间
不可变——`repository`、`mounted_at`、`cache` 配置本就不随时间变化；
`head` 字段在挂载后分支切换/HEAD 移动时**不**跟随更新（需探测实时 HEAD
请进入 `HEAD/` 目录或直接 `git rev-parse`）。它不参与 refs 指纹失效
机制，重新挂载即刷新。

### 3.7 CLI 与退出码

程序以 **mount(8) 助手** 形态发布：可执行文件安装为 `$(sbindir)/mount.gitfs`
（命名遵循 `mount.<fstype>` 约定），三种调用形态等价：

```
mount -t gitfs <repo> <mountpoint> [-o <opts>]   # 经 mount(8) exec 助手
mount <mountpoint>                                # /etc/fstab 条目触发
mount.gitfs <repo> <mountpoint> [选项]            # 直连调用
```

**mount(8) 实际转交行为（经 util-linux 2.42.3 libmount 源码并真实
mount(8) exec 助手实测逐条核验，Q21 勘误 Q13(a)，Q22 勘误 Q21 的
VFS 键滤除与 -N 转交口径，Q26a 补契约末位 `-t` 子句）**：助手契约为
`[-sfnv] [-N namespace] [-o options] [-t type.subtype] <src> <dir>`
（man mount(8) EXTERNAL HELPERS 节明载末位 `-t` 子句：带点的 fstype
`type.subtype` 由 libmount 以 `-t <type.subtype>` 整值转交——实测
带点 fstype 的助手 argv 确为 `<src> <dir> -o rw -t <type.subtype>`，
无点则恒不出现；`gitfs` 无点，经 mount(8) 的路径永不收到该子句，
但契约核验口径为穷尽、缺此子句即非穷尽，故钉住处理：若经直连调用
收到 `-t gitfs` 则接受并记 verbose 一条（冗余自指），**其余任何值**
（含 `gitfs.<x>` 带点形态——用户 `mount -t gitfs.<x>` 时 libmount
确会 exec `mount.gitfs` 并转交该值）为参数错误退出 1：fstype 错配，
Q26a），libmount exec
助手时转交契约短选项 `-s/-f/-n/-v`（`--fake` 转交为 `-f`）、
`--namespace` 转交为 `-N <ns>`（与 `-n`/`-s` 同列容忍忽略，Q22b；
`-s` 仅标志本身被容忍、不放松未知 `-o` 键拒绝，见下 -n/-s/-N
帮助项与 §7.13(b)，Q23b）
与 `-o` 选项串；`-r`/`-w` **从不以标志形式转交**，而是由 libmount
并入 `-o` 串（`-r` → 追加 `ro`、`-w` → 追加 `rw`）；且对未显式
只读（`-r` 或 `-o ro`）的调用，libmount **无条件在 `-o` 串中预置
`rw`**——裸调用 `mount -t gitfs <repo> <dir>` 实际到达助手的 argv
为 `<repo> <dir> -o rw`，且选项恒出现在位置参数**之后**（实测如
`<src> <dir> -f -o rw`），参数解析须容忍选项后置（GNU getopt 式
置换，Q22c），否则主路径直接破；`rw` 必须被接受（见下选项说明），
否则 §0/本节首推的默认调用形态必然 exit 1。`-o` 串内的
rw/ro 冲突（如 `-o rw,ro`）经 mount(8) 时由 libmount 折叠为单个
键（实测后者胜：`rw,ro` → 单个 `ro`），助手不会从 mount(8)
同时收到 rw 与 ro——fstab `ro` + CLI `-o rw` 的覆盖流同样在
libmount 层裁决为单个到达键、到达后均为无操作（`rw` 记警告），
挂载恒为 ro（硬编码基线），实现者无须为该组合写特殊解析（直连
调用纵然出现 rw,ro 同串，两键亦各自为无操作，Q24c）。fstab 条目
`/path/repo  /mnt/gitfs  gitfs  ro,blob-cache-size=128  0  0` 同样经由
助手挂载；util-linux ≥2.35 还允许 CLI `-o` 在 fstab 选项之上增改
（合并串中同一键可出现两次），相应覆盖语义见下"后者胜"规则。
**`-o` 串中 VFS 键的实际到达口径（Q22a，勘误 Q21 版正文的断言——
"通用 VFS 键由 mount(8) 翻译为挂载 syscall 标志、不会到达 gitfs"
系事实错误；Q23a 补全实测名单并钉分类规则与键名匹配；Q24a 再补
symfollow/nosymfollow/nouser 并修分诊规则锚点；Q25a 勘误
acl/quiet/showexec/bsdgroups 的表内归属并限定表轨）**：libmount
仅滤除固定子集（auto/noauto/comment=/x-*/loop/offset=/sizelimit=/
defaults 及传播键），其余 VFS 键原样到达——atime/noatime/relatime/
strictatime/lazytime/diratime/nodiratime、sync/async/dirsync、
exec/noexec、user/users/owner/group/nouser、symfollow/nosymfollow、
iversion/silent/loud/mand/nomand/nofs、_netdev、nofail、remount、
uid=/gid=/umask=、context=/fscontext=/defcontext=/rootcontext=
（SELinux 标签键，值含冒号、实测整值到达）、acl/quiet/showexec/
bsdgroups 均在列——后四键**不在** man mount(8)
"Filesystem-independent mount options" 表内（实测 2.42.3：quiet/
showexec 属 FILESYSTEM-SPECIFIC 节的 FAT 选项、acl 属 ext/ntfs/
overlay 等 fs-specific 节、bsdgroups 全 man 页零出现），其到达
系 libmount 将表外未知键当 fs-specific 数据转发所致，故收入
枚举名单而非表轨（Q25a 勘误 Q24a 的"表内无操作键、表轨兜底"
断言）。到达的无关 VFS 键按 `rw`/ntfs-3g
先例**接受并忽略**（verbose 记一条）：atime 族/sync 族/exec 族/
user 族（含 nouser）/symfollow/nosymfollow/iversion/silent/loud/
mand/nomand/nofs/_netdev/nofail/acl/quiet/showexec/bsdgroups
（后四键为表外转发键，Q25a）——
ro、atime≡mtime 语义下均无害，`mount -t gitfs -o
noatime,nodiratime repo dir` 这类常见习惯、fstab 的 `user`
（非 root 挂载）与 boot 常见的 `nofail`/`_netdev` 均不因未知键
失败；`suid`/`dev` 维持报错退出 1（实测确实到达，规范可测）；
`remount`、`uid=`/`gid=`/`umask=` 及 `context=` 族显式定口径为
报错退出 1 并给专用错误文案（v0.1 无 remount/属主映射/安全标签
语义，静默忽略会掩盖用户意图——SELinux 环境以 `context=` 挂载
是常规操作，须显式失败而非静默丢标签，与 `uid=` 同理，Q23a）。

**到达键的分诊规则（Q23a 钉规则；Q24a 改双轨锚点——原"表内/
表外"单轨与枚举快照不自洽且自身会漏：`nofs` 在枚举接受名单内、
却不在 util-linux 2.42.3 man 表内，按表外条款会误落 libfuse 拒
绝；演进键 `symfollow`（2.41+ 新增、实测原样到达、对 gitfs 为
无操作）不在表内亦不在枚举内，其逆键 `nosymfollow`（表内）却
被接受；Q25a 再勘误 Q24a——`acl`/`quiet`/`showexec`/`bsdgroups`
四键实测到达但均不在 man 表内，原"表内无操作键由表轨兜底"的
归位错误，四键改入枚举轨、表轨限定仅认该表所列键）**：
(1) 名单匹配按**截首个 `=` 取键名**钉住——
`user=alice`、`nofail=1` 实测带值到达，按键名归入相应分诊、
值不再校验（`context=` 族值含冒号，逗号切分不受影响、仅不得
再按冒号拆分值）；该规则同样吸收 util-linux 2.41+ 的 ro/rw 带
限定值形态（`ro=vfs`/`rw=fs` 及 recursive 参数形态）——截 `=`
后键名即 ro/rw，落入既有无操作路径（ro 冗余、rw 记警告），
无须特殊解析（Q25d）；(2) **枚举轨优先**——上列实测枚举名单所列
键（含表外的 `nofs`/`symfollow` 与 `acl`/`quiet`/`showexec`/
`bsdgroups`）一律按名单分诊为接受并忽略、verbose 记一条；
(3) **表轨兜底**——仅认 man mount(8)
"Filesystem-independent mount options" 一节**所列**的键：
所列且对 gitfs 为无操作者（ro 基线、atime≡mtime、无属主映射
语义下无效果；表内否定键 `noiversion`/`norelatime`/
`nostrictatime`/`nolazytime` 与带值形态（`noexec=recursive`
等）正是由本轨经截=取键名吸收——枚举名单未逐一收录它们，
在此点破防再犯）→ 接受并忽略、verbose 记一条；
该表中对 gitfs 有语义或安全影响者（suid/dev、remount、uid=/
gid=/umask=、context= 族）→ 专用错误退出 1；(4) 两轨皆不属的
键透传 libfuse（未知 → 退出 1）——直连调用收到 mount(8) 路径
上被滤除的键（`defaults`/`auto`/`noauto`/`comment=`/`x-*`/
`loop`/`offset=`/`sizelimit=`/传播键）时即落此条、退出 1：这些
是 mount(8) 侧指令（fstab 自动化/回环设备/传播设置），直连
路径无消费者，**显式声明该不对称**为设计意图——mount(8) 路径
滤除后恒成功，直连路径报错以暴露无效意图（Q25c）。
枚举名单是 util-linux 2.42.3
的实测快照，对表内键不宣称穷尽（表轨兜底）；**演进条款**：
未来 util-linux 版本使新的表外键到达 `-o` 串时（如 2.41+ 的
`symfollow`），按"是否对 gitfs 无操作"人工分诊入枚举名单，并
写入维护核对单 `docs/maintenance-checklist.md`（§4，新
util-linux 发布 → diff man 表与 libmount 转发键集 → 分诊 →
更新枚举名单与 §4 用例），而非因枚举缺漏
落入 libfuse 拒绝（连续四轮 review 均发现枚举或锚点漏键，故钉
双轨锚点 + 演进条款防再漏）。

**user 族实测附带键（Q23d 措辞精化）**：`user`/`users` 隐式附带
`noexec,nosuid,nodev` 全三项，`owner`/`group` 仅附带
`nosuid,nodev`，`user=<name>` 不附带任何键——三者对硬编码基线
（nosuid/nodev）均为无操作或冗余、行为不受影响，但 §4 对
owner/group 不得写 noexec 存在性断言。直连调用时出现在 `-o`
中的其余键则透传 libfuse（见下）。

```
用法: mount.gitfs [选项] <repository> <mountpoint>
      （选项可出现在位置参数之后——mount(8) exec 助手实测 argv 为
        <repo> <dir> -f -o rw，解析须容忍 GNU getopt 式选项置换，Q22c）

选项:
  -o OPT[,OPT…]       键值/开关形态。gitfs 自有键 blob-cache-size=<MiB>、
                       tree-cache-size=<MiB>（连字符与下划线拼法等价），
                       与同名长选项语义、校验完全一致；同一键重复给出
                       取"后者胜"（出现序：fstab 条目 → 命令行选项按
                       命令行先后，-o 串内从左到右），对齐 mount(8) 的
                       fstab+CLI 合并惯例（util-linux ≥2.35，Q21c），
                       不因该覆盖流退出 1；与硬编码基线同义的键（ro/
                       nosuid/nodev/default_permissions/use_ino）接受为
                       冗余无操作；`rw` 同样接受为无操作并记 stderr
                       警告——libmount 对非只读调用无条件预置该键
                       （见上，Q21a），ro 为硬编码基线、安全不被稀释
                       （ntfs-3g 同先例）；到达 `-o` 串的无关 VFS 键
                       （atime/noatime/relatime/strictatime/lazytime/
                       diratime/nodiratime、sync/async/dirsync、
                       exec/noexec、user/users/owner/group/nouser
                       （user/users 附带 noexec,nosuid,nodev、
                       owner/group 仅 nosuid,nodev、user=<name> 无
                       附带，Q23d）、symfollow/nosymfollow、
                       iversion/silent/loud/mand/nomand/nofs、
                       _netdev、nofail、acl/quiet/showexec/
                       bsdgroups（表外转发键，Q25a），
                       Q22a/Q23a/Q24a/Q25a）一律接受并
                       忽略、verbose 记一条——名单匹配按截首个 = 取
                       键名钉住（user=alice、nofail=1 按键名归入，
                       且 2.41+ 的 ro=vfs/rw=fs 带值形态截=后即
                       ro/rw、落入既有无操作路径，Q25d），分诊为
                       双轨锚点（Q24a，Q25a 勘误）：枚举名单
                       优先，mount(8) 通用 VFS 选项表所列键中其余
                       对 gitfs 无操作者（如 noiversion/norelatime/
                       nostrictatime/nolazytime 等表内否定键）兜底
                       接受忽略、表轨仅认该表所列键，表外新键（如
                       util-linux 2.41+ 的 symfollow）人工分诊入
                       枚举名单、未分诊的表外键透传 libfuse，防
                       util-linux 演进再漏键；直连调用收到
                       defaults/auto/noauto 等 mount(8) 侧滤除键
                       报错退出 1（显式不对称，Q25c）；ro、
                       atime≡mtime 语义下忽略无
                       害（`-o noatime,nodiratime` 习惯用法与 fstab
                       `user`/`nofail`/`_netdev` 场景不因未知键
                       失败）；反向键 suid/dev 报错退出 1，
                       防安全基线被 CLI 稀释或顶掉
                       （Q10/Q13/Q14/Q21）；remount 与 uid=/gid=/
                       umask= 及 SELinux 标签键 context=/fscontext=/
                       defcontext=/rootcontext=（值含冒号）报错退出 1
                       并给专用错误文案（v0.1 无 remount/属主映射/
                       安全标签语义，静默忽略会掩盖用户意图，
                       Q22a/Q23a）；`fsname=` 允许透传覆盖基线
                       （cosmetic），`subtype=` 覆盖基线 → 参数错误
                       退出 1（基线保护，Q15a）；其余键原样透传 libfuse
                       选项解析器（如 kernel_cache；allow_other 需
                       /etc/fuse.conf 启用 user_allow_other），未知键由
                       libfuse 拒绝 → 退出 1
  --blob-cache-size <MiB>  blob LRU 缓存上限（默认 64）；值为正整数
                       （十进制 MiB）——0、负数、非数字或溢出 size_t →
                       参数错误（退出码 1）；不设人为上限，受可用内存约束
  --tree-cache-size <MiB>  libgit2 对象缓存（tree/commit）总预算上限
                       （默认 256，即 libgit2 默认值）；校验规则与
                       --blob-cache-size 相同；与 blob 缓存各自独立记账
                       （见 3.5、Q13）
  --foreground         前台运行（默认守护进程化）；仅长选项形态——
                       短选项 -f 已按助手契约保留给 fake（Q21b）
  -f                   fake（mount(8) 的 --fake 转交形态：libmount 的
                       exec_helper 即向助手传 -f，Q21b）：完整校验参数
                       与全部选项、并同样打开仓库校验可读性（坏仓库
                       → 退出 2，Q23c——否则 mount --fake 会给 boot
                       时才失败的 fstab 条目开绿灯），仅跳过挂载与
                       守护进程化，校验通过后退出 0
  --verbose / -v       输出路径解析与缓存命中日志，及 blob 全量解压
                       事件（每次解压一条，即解压计数：超限 open-pin
                       与缓存 miss 装载各一条，见 3.5）
                       （mount(8) 的 -v 映射至此）
  -n / -s / -N <ns>    mount(8) 转交的 no-mtab / sloppy / namespace 标志：
                       容忍并忽略（助手契约为 [-sfnv] [-N namespace]
                       [-o options] [-t type.subtype]，--namespace 时助手收到 -N <ns>，
                       Q22b——本机 mount(8) 在 exec 前拒绝切 namespace，
                       该转交路径依 man 页契约文档化；-s 仅指标志本身
                       被容忍、不放松未知 -o 键拒绝——mount(8) 文档
                       语义为"忽略文件系统不支持的挂载选项"，但静默
                       吞掉拼写错误的自有键会掩盖配置错误，故显式
                       文档化不采用该放宽，未知键仍按上表分诊，
                       Q23b；-r/-w 从不以标志形式转交——libmount 并入 -o
                       串为 ro/rw，见上；直连调用显式传 -r/-w 按未知
                       选项处理 → 参数错误退出 1）
  --version / --help
```

- 挂载选项硬编码基线：`ro,fsname=gitfs,default_permissions,subtype=gitfs,
  nosuid,nodev,use_ino`（`use_ino` 令内核采用 gitfs 填充的路径派生
  `st_ino`，见 3.2）。透传键与基线的冲突序（Q15a）：`fsname=` 可被
  用户 `-o` 覆盖（cosmetic，仅影响展示名）；`subtype=` 与 ro/nosuid/
  nodev/default_permissions/use_ino 同列受基线保护——覆盖即参数错误
  退出 1，`/proc/mounts` 的 type 字段与 `mount -t gitfs`/`findmnt
  -t gitfs` 的匹配依赖它。
- 卸载：`umount <mountpoint>`（或 `fusermount3 -u`）；守护进程收到
  `SIGINT`/`SIGTERM` 亦优雅退出（见 3.4）。
- 退出码：0 挂载成功（守护进程化后父进程退出）或前台模式优雅终止
  （SIGINT/SIGTERM，见 3.4），`-f` fake 校验通过后不挂载退出亦为
  0（校验含仓库可读性，坏仓库 → 退出 2，Q23c）——mount(8) 助手
  形态下 mount(8) 传播的 exit 0 语义即"挂载成功"，
  卸载由 umount(8)/fusermount3 完成、不经本程序（Q22d 勘误原
  "0 正常卸载"措辞）；1 参数错误；2 仓库不可读/不是 git 仓库；
  3 挂载失败。

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
├── CMakeLists.txt            # >= 3.16，C++17，-Wall -Wextra -Wpedantic -Werror(CI)；
│                             # install: $(sbindir)/mount.gitfs + $(mandir)/man8
├── LICENSE                   # GPL-3.0-or-later（SPDX 标注同左）
├── README.md                 # 快速开始、语义说明（含 /commits 首次 open
│                             # 的停顿语义，见 3.4；超限 blob open 的
│                             # 等待语义与锁外解压不阻塞他请求声明，
│                             # 见 3.5；可缓存 blob LRU 装载锁外解压
│                             # （瞬时重复解压可能）声明，见 3.5；
│                             # 挂载期间禁 gc 的运维提示，
│                             # 见 3.1；st_ino 注册表内存上界与不回收
│                             # 声明，见 3.2）、FAQ
├── CONTRIBUTING.md           # 分支/提交规范、如何跑测试
├── CODE_OF_CONDUCT.md
├── SECURITY.md               # 报告漏洞渠道
├── CHANGELOG.md              # Keep a Changelog 格式
├── .clang-format             # Google 风格（决议 Q6）
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
│                              # 与 refs/remotes/origin/HEAD（供隐藏断言）、
│                              # refs/replace/<oid>（供 replace 不生效断
│                              # 言）、refs/notes/keep 与 refs/stash
│                              # （供 commits 入集口径断言，对齐
│                              # rev-list --all oracle）、update-ref
│                              # 指向 blob 的分支 ref（供非 commit
│                              # 目标入口 ENOENT 断言，见 3.1/Q17b）
└── docs/
    ├── filesystem-semantics.md  # 对用户承诺的语义（本文 3.x 的稳定化版本；
    │                          # 必含"gc/prune 并发""空仓库""readdir 字节
    │                          # 字典序"三节，见 3.1、3.2；含 st_ino 注册
    │                          # 表内存上界声明）
    ├── maintenance-checklist.md  # 维护核对单：3.7 演进条款的载体（新
    │                          # util-linux 发布 → diff mount(8) man
    │                          # "Filesystem-independent mount options"
    │                          # 表与 libmount 转发键集 → 按"是否对
    │                          # gitfs 无操作"分诊 → 更新枚举名单与
    │                          # §4 用例，见 3.7/§7.14(a)、Q25b）
    └── mount.gitfs.8            # man 手册（roff；CMake install 到 $(mandir)/man8）
```

- **提交规范**：Conventional Commits（`feat:`/`fix:`/`docs:`…），CI 校验。
- **版本**：SemVer；打 tag 出 release，CI 产出各发行版二进制 + SBOM。
- **测试策略**：
  - 单测：Catch2 v3；覆盖 path_map 状态机（含畸形路径、Unicode、超长 oid）、
    LRU 逐出、错误映射表、st_ino 注册表碰撞消歧（哈希函数可注入，
    强制构造碰撞，见 3.2/Q15c）；
  - 集成：脚本生成 fixture 仓库 → 挂载到 tmpdir → 断言内容与
    `git --no-replace-objects ls-tree`/`git cat-file` 结果一致（oracle
    与 gitfs 同为"replace 不生效"语义，见 3.1；fixture 含 replace ref
    的反例断言）；含"挂载后新增 tag 立即可见"的一致性用例；
    边缘用例：`'.'`/`'..'` entry 在 readdir/getattr 被跳过且记警告、
    `refs/remotes/origin/HEAD` 在 `/remote` 不可见（访问 → `ENOENT`）、
    嵌套 ref 分组/命名空间目录（`/branch/feature`、`/remote/origin`）
    的元数据断言：mode 0755、nlink=2、mtime/ctime/atime = 挂载时刻
    （与入口目录同口径、非 committer time），及根与 tree 目录
    `st_size`=4096（Q26b/Q26d）、
    非 UTF-8 文件名按原始字节读回、含嵌入 NUL 的 symlink 截断至首个
    NUL、detached HEAD 独有 commit 出现在 `commits` 清单中、存在
    blob tag 时 `commits` 仍可成功生成且含全部 commit oid（非
    commit ref 跳过不计错，见 3.5）、`commits` 清单排序归一后与
    `git --no-replace-objects rev-list --all` 输出一致（notes/stash
    ref 的 commit 在列）、readdir 顺序断言（根与 branch/tag/remote/
    tree 按分量原始字节字典序，含嵌套 ref 分组前缀与
    `.gitfs-submodule` 合成项；oracle 以 `LC_ALL=C sort` 归一比对）、
    非 commit 目标入口：ref 指向 blob 的分支（update-ref 手工构造）
    → `/branch/<name>` `ENOENT`（与 `/tag` 同口径，见 3.1/Q17b）、
    超限 blob 直读（`--blob-cache-size=1` 挂载下读取大于容量的
    blob，另以一个恰等于容量上限（1 MiB = 1048576 字节）的 blob 断言
    同走绕过路径——边界 `>=` 钉住，Q26d；顺序分片整读、内容逐块
    与 `git cat-file` 一致且既有缓存
    条目不被逐出；open-pin 语义保证单次顺序读只解压一次——以
    **verbose 日志的解压计数 = 1** 直接断言（每次全量解压记一条
    `-v` 日志，见 3.5/Q18b；耗时比值断言只能抓 O(n²) 回归，计数
    断言补足其无法排除多次解压的盲区），另以“整读耗时不随分片
    数平方增长”的宽松比值断言在自管 runner 上作参考检查（共享
    runner 抖动大，不作门禁），见 3.5/Q17a；锁外解压另设可选
    参考断言：超限 blob open 解压期间并发的根 readdir 不被长时
    间阻塞（自管 runner、宽松阈值），见 3.4/Q18a）；getattr 零解压
    断言：`-v` 挂载下对含大 blob 的目录做 `ls -l`（getattr 全遍历），
    verbose 日志不出现任何解压事件（blob 尺寸经 header-only 读取
    获得、getattr 不触发全量解压，见 3.2/Q19b——与“getattr 不触
    发 revwalk”断言同构）；可缓存 LRU 装载的锁分段另设可选参考
    断言：大容量 `--blob-cache-size` 挂载下首次读大可缓存 blob 期
    间并发的根 readdir 不被长时间阻塞（自管 runner、宽松阈值，见
    3.5/Q19a）；
    **mount(8) exec 路径九场景（Q21，Q22 扩至五，Q23 扩至七，Q24 扩至九）**：经真实
    `mount -t gitfs`（需 util-linux ≥2.35 与特权环境，CI 无特权时
    skip 标记）走 libmount exec_helper 全链路：(1) 默认调用
    `mount -t gitfs <repo> <dir>`
    （不带 -o ro）——libmount 预置的 `-o rw` 到达助手，断言被接受
    （stderr 记警告）且挂载成功、卸载干净；(2) `mount --fake -t gitfs
    <repo> <dir>`——断言转交的 `-f` 走 fake 语义：参数、选项与
    仓库可读性完整校验通过（另以坏仓库断言 `-f` 退出 2，Q23c）、
    mountpoint 未被挂载、退出码 0；(3) fstab 条目
    `blob-cache-size=128` + CLI `-o blob-cache-size=256` 合并调用——
    断言自有键后者胜（挂载后读 `.gitfs.json` 的 `cache.blob_bytes`
    = 256×1024²）；(4) `mount -t gitfs -o noatime <repo> <dir>`——
    断言到达的 noatime 被接受并忽略（verbose 记一条）且挂载成功、
    卸载干净（Q22a）；(5) fstab 含 `user` 的条目以非 root 用户
    `mount <dir>` 触发——断言 `user` 及其隐式附带的
    noexec,nosuid,nodev（实测 user 附带全三项，Q23d）到达后均被
    接受（noexec 忽略、nosuid/nodev 冗余无操作）且挂载成功
    （Q22a；对 owner/group 不写 noexec 存在性断言，Q23d）；
    (6) `mount -t gitfs -o noatime,nodiratime <repo> <dir>`——经典
    fstab 组合，断言两条均原样到达且按键名匹配被接受并忽略
    （verbose 记两条）且挂载成功、卸载干净（Q23a）；(7) `-o
    user=alice` 键值形态（fstab `user=alice` 或 CLI `-o
    user=alice`）——断言带值到达、按"截首个 = 取键名"匹配入忽略
    名单（实测不附带任何隐式键，Q23d）且挂载成功（Q23a）；(8)
    `mount -t gitfs -o symfollow <repo> <dir>`——util-linux 2.41+
    演进键（man 表外、枚举轨收录），断言原样到达且被接受并忽略
    （verbose 记一条）且挂载成功、卸载干净（Q24a）；(9) `-o
    nouser`（man 表内无操作键）——断言原样到达且被接受并忽略、
    挂载成功、卸载干净（Q24a）；另以直连调用断言 `-o remount`、`-o uid=1000` 与 `-o
    context=system_u:object_r:user_home_t:s0`（值含冒号、整值
    到达）退出 1 且错误文案专用（Q22a/Q23a）；
    另增 st_blocks 断言：`du`（512B 块口径）对普通
    文件与合成文件报块数 = ceil(st_size/512)，不再恒为 0
    （见 3.2/Q21d）；
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
  `rw/suid/dev`（见 3.3、3.7）（`rw` 经 §7.11(a)/Q21 勘误改为接受为
  无操作并记 stderr 警告——libmount 对助手调用无条件预置该键；现行
  拒绝名单为 `suid/dev`；到达 `-o` 串的无关 VFS 键（noatime 族等）
  经 §7.12(a)/Q22、§7.13(a)/Q23（补全名单并钉分诊规则）、
  §7.14(a)/Q24（再补名单并改双轨锚点）、§7.15(a)/Q25（四键改入
  枚举轨、表轨限定仅认表内所列）定为接受并忽略、
  remount/uid=/context= 等无对应语义的键退出 1）。
- **Q11 libgit2 缓存调参与防双层缓存（已决）**：tree/commit 依赖
  libgit2 内置缓存，但显式抬 per-type 上限（tree 1MiB——默认 4KiB 会
  漏掉大目录 tree，拖垮元数据密集负载）；blob per-type **写死 0**，
  保证解压后 blob 仅存于自家 LRU、不双层缓存双记账；总预算维持默认
  256MiB，与 `--cache-size` 独立记账（见 3.5；选项名与可调性经 Q13
  调整为 `--blob-cache-size`/`--tree-cache-size`）。

### 7.2 勘误与语义补全（2026-09-24，review round 2 跟进）

- **Q12 文档一致性与运维语义（已决）**：(a) 时间线勘误——RFC 头部与 §7
  的日期由 2025-09-24 统一为 **2026-09-24**（与仓库提交、§7.1 及 §3.6
  示例时间戳一致），man 页头注释同步更新至 Q1-Q12；(b) §3.5 清单体
  积估算限定口径——45MB 仅按 sha1（41B/行），sha256 为 65B/行
  ≈ 74MB（该数值后经 Q16(f) 勘误为 71.5MB）；(c) §3.2 子模块说明文件命名明确为 `<name>.gitfs-submodule`；
  (d) 显式声明挂载期间外部 `git gc`/`git prune` 语义——对象被删 →
  瞬时 `ENOENT`/`EIO`，建议挂载期间禁 gc 或接受瞬态错误（见 3.1）；
  (e) 显式声明空仓库行为——挂载成功、入口为空（见 3.1）；(f)
  `--cache-size` 校验规则定案：正整数，0/负数/非法 → 退出码 1，无
  人为上限（见 3.7）。README 与 filesystem-semantics.md 大纲补
  "gc 并发""空仓库"内容（见 §4）。

### 7.3 CLI 形态与缓存参数（2026-09-24，review round 4 跟进）

- **Q13 mount(8) 助手形态与缓存参数（已决）**：(a) 可执行文件更名为
  `mount.gitfs`（安装 `$(sbindir)`），`mount -t gitfs`、fstab 条目与直连
  三种调用等价；mount(8) 转交标志映射定案（`-r` 接受、`-w` 报错退出 1、
  `-n`/`-s` 容忍忽略、`-v` 映射 verbose，`--fake` 由 mount(8) 自身消化）
  ——该转交口径基于错误前提，经 §7.11(a)(b)/Q21 全面勘误：libmount
  实际仅转交 `-s/-f/-n/-v` 与 `-o` 串（`--fake` → `-f`；`-N <ns>`
  转交经 §7.12(b)/Q22 补入容忍忽略），`-r`/`-w`
  并入 `-o` 串为 `ro`/`rw` 且非只读调用无条件预置 `rw`，故 `rw` 接受
  并记警告、`-f` 实现为 fake、前台仅 `--foreground`；
  `-o` 增加键值形态，自有键 `blob-cache-size`/`tree-cache-size`（连字符/
  下划线等价）与同名长选项同语义，同一键重复给出 → 退出 1（该口径经
  §7.11(c)/Q21 修订为按出现序后者胜）；(b)
  `--cache-size` 更名 `--blob-cache-size`，新增 `--tree-cache-size`
  （默认 256MiB，即 libgit2 对象缓存默认总预算，映射
  `GIT_OPT_SET_CACHE_MAX_SIZE`），校验规则沿用 Q12(f)：正整数、0/负/
  非法/溢出 → 退出 1，无人为上限，与 blob 缓存独立记账（见 3.5、3.7）；
  (c) 修订 Q10 黑名单：与基线同义的键（ro/nosuid/nodev/
  default_permissions）接受为冗余无操作（fstab `ro,...` 场景需要），
  反向键（rw/suid/dev）仍报错退出 1（`rw` 经 §7.11(a)/Q21 再修订为
  接受并记警告）；(d) `.gitfs.json` 的 `cache_bytes`
  扩展为 `cache` 对象（blob/tree 双口径字节，见 3.6）；(e) man 页更名
  `docs/mount.gitfs.8`（man8 章节，mount 助手惯例），头注释同步 Q1-Q13。

### 7.4 定稿补全（2026-09-24，定稿前 review 跟进）

- **Q14 inode、清单确定性与瞬态失败语义（已决）**：(a) `st_ino` 由完整
  VFS 路径字节的 64 位稳定哈希派生（合成入口同口径、根固定为 1），跨
  重挂载确定；**不**按 oid 派生，防止 `tar`/`rsync -H`/`diff` 把多个
  ref 路径下的同一 blob 误判为硬链接；挂载基线增补 `use_ino`，`-o`
  同义冗余键名单相应补入（见 3.2、3.7）；(b) `commits` 拓扑序的确定性
  钉住实现：`GIT_SORT_TOPOLOGICAL` + 完整 ref 名字典序排序入队
  （HEAD 恒最后），并列 commit 出队次序由入队序唯一决定，清单跨重
  挂载逐字节可复现（见 3.1、3.5）；(c) `commits` 生成期间对象被
  gc/prune 删除致 revwalk 中途失败：该次 `open` 返回 `EIO`、半成品
  不缓存、指纹不更新、下次 `open` 重试，与 3.1 瞬态错误口径一致
  （见 3.1、3.5）；(d) xattr 族（get/set/list/remove）不注册 →
  `ENOTSUP`，SELinux 环境 statx 的 `security.*` 查询命中即干净短路
  （见 3.3）；(e) `-o kernel_cache` 澄清：仅影响数据页缓存，
  attr/entry timeout 仍恒 0，元数据陈旧窗口须另显式传
  `attr_timeout`/`entry_timeout`（见 3.5）。
- （记账）round 3 仅为 §4 ASCII 目录树第 349 行对齐微修（提交
  8e0e57d），无决议内容，补记于此。

### 7.5 收尾决策（2026-09-24，定稿 review round 2 跟进）

- **Q15 透传键覆盖序、commits 生成前 mtime、st_ino 碰撞消歧（已决）**：
  (a) `-o` 透传键与硬编码基线的冲突序——`fsname=` 允许覆盖（cosmetic）；
  `subtype=` 入基线保护名单，覆盖 → 参数错误退出 1（`/proc/mounts`
  的 type 与 `mount -t gitfs`/`findmnt -t gitfs` 匹配依赖；与
  ro/nosuid/nodev/default_permissions/use_ino 保护同口径）（见 3.7）；
  (b) `commits` 未生成窗口（`st_size=0` 阶段）的 `st_mtime` 为**挂载
  时刻**（与 `.gitfs.json` 同口径，不逐次取当前时钟），首次 open 后
  跳变为生成时刻；指纹重建后 stat 报当前缓存版本的生成时刻，钉住
  旧版本的句柄可见更新的 mtime（见 3.2）；(c) `st_ino` 哈希碰撞由
  挂载期注册表消歧：`primary_hash → 首个占用路径`，冲突时后到路径
  取 `H(path || '#' || k)` 最小未占用值（k=1,2,…），保证不同路径
  必得不同 inode；跨重挂载可复现性在冲突路径上除外（10⁶ 条目生日
  碰撞概率 ≈ 3×10⁻⁸），哈希函数可注入、单测强制碰撞断言消歧
  （见 3.2、§4）。

### 7.6 收敛补全（2026-09-24，review round 1/5 跟进）

- **Q16 /commits 入集口径、注册表上界、readdir 顺序、合成元数据与
  超限 blob（已决）**：
  (a) `/commits` 入队集合钉住：枚举 `refs/` 下**全部** ref（heads/tags/
  remotes 之外，notes、stash、replace 等特殊命名空间一并纳入），对齐
  `git --no-replace-objects rev-list --all` 的 oracle（实测：该集合
  git 同样全量枚举，非 commit 目标被静默跳过）；另加 HEAD；逐个
  peel 至 commit，peel 失败者（如轻量 tag 指向 blob/tree）跳过并记
  verbose 日志、不算错误，绝不因个别非 commit ref 使清单生成失败或
  退化为 `EIO`；§4 增"存在 blob tag 时 commits 仍可生成"与
  "notes/stash ref 的 commit 在列"用例；
  (b) `st_ino` 碰撞注册表显式声明内存上界：条目懒注册（仅实际解析过
  的路径入表），占用 ∝ 挂载期内被触及的不同路径数，条目挂载期内
  不回收（inode 永不复用），README 与 filesystem-semantics.md 同步
  声明；
  (c) readdir 顺序全域钉住为**分量原始字节字典序**：branch/tag/remote
  与根同规则；tree 目录**重排**为字典序而非沿用 git tree 内在序
  （目录名附加 `/` 的比较规则，与纯字节序在 `foo`/`foo.txt` 组合上
  相反），`.gitfs-submodule` 合成项同名参与排序；§4 增顺序断言；
  (d) 合成入口元数据补全：`commits`/`.gitfs.json` 为 `S_IFREG|0444`、
  `nlink=1`；根与 `/branch`、`/tag`、`/remote`、`/commit`、`/HEAD`
  入口目录的 mtime/ctime/atime = 挂载时刻（`nlink=2` 通用规则；该
  口径经 §7.16(b)/Q26 扩展至合成分组/命名空间目录、§7.16(d) 补目录
  st_size=4096）；
  (e) 超限 blob（单 blob > `--blob-cache-size`，该边界经 §7.16(d)/Q26
  钉为 `>=`）绕过缓存直读：不插入、
  不逐出既有条目；解压时机经 §7.7(a) 细化为 open-pin 一次性解压
  （原“逐 read 直取、内核页缓存兜底”表述不能消除重复解压，§3.5
  已相应修订）；blob 装载在 3.4 单互斥下串行执行，天然
  single-flight（该口径经 §7.8(a) 收窄为可缓存装载，又经 §7.9(a)
  修订为锁外解压+回锁双检复用，不再承诺严格 single-flight，超限
  解压移至锁外）；§4 增超限 blob 读用例；
  (f) 小项：`.gitfs.json` 的 `repository` 对非法 UTF-8 字节按 `%XX`
  百分号转义（保证 JSON 合法 UTF-8）；`/commit/<oid>` 仅接受小写
  十六进制（大写 → ENOENT）；§3.5/§7.2(b) 的 sha256 清单估算由
  74MB 勘误为 71.5MB（65B × 110 万）。

### 7.7 解压时机与非 commit 入口口径（2026-09-24，review round 2/5 跟进）

- **Q17 超限 blob 解压时机、非 tag 入口 peel 口径与文档勘误（已决）**：
  (a) 超限 blob（单 blob > `--blob-cache-size`）的直读钉住为
  **open-pin 一次性解压**：`open` 时一次 `git_blob` lookup +
  全量解压（锁边界经 §7.8(a) 细化：lookup 在单互斥内、全量解压
  移至锁外），解压块钉住于 `fi->fh`、`release` 释放——与
  `commits` 的 open-pin 同构，单次 open 的顺序分片读只解压一次，
  消除逐 read 重解压的 O(size²) 放大；内核页缓存只覆盖 mmap 的
  pack 原始字节、不含解压结果，不能作为免重解压的依据（§3.5
  原表述已修订）；内存峰值显式声明：Σ 并发未 `release` 的超限
  blob open 所持 blob 大小，v0.1 不做跨 open 共享，流式按需解压
  留作后续优化（见 3.3、3.5）；
  (b) `/branch/<name>`、`/remote/<remote>/<b>`、`/HEAD` 的 ref 目标
  非 commit（手工 `git update-ref` 可构造）时与 `/tag` 统一口径：
  `ENOENT` + 警告日志，v0.1 不呈现非 commit 目标（见 3.1）；
  (c) 勘误：§7.6(d) 入口目录列表的双斜杠排版笔误修正；§3.6 补
  `%XX` 转义“仅保证 JSON 合法、不承诺可逆”声明（字面 `%XX` 与
  转义产物不可区分）；
  (d) §4 增用例：非 commit 目标分支入口 `ENOENT`、超限 blob 顺序
  分片整读（内容逐块比对 + “整读耗时不随分片数平方增长”的参考
  断言，见 §4）。

### 7.8 锁分段与解压可观测性（2026-09-24，review round 3/5 跟进）

- **Q18 超限 blob open-pin 的锁边界与解压计数（已决）**：
  (a) §7.7(a) 的超限 blob open-pin 细化**锁分段**：单互斥内仅完成
  `git_blob` lookup/句柄获取，**全量解压移至锁外执行**——数 GB blob
  解压需数秒，整段持锁会让全挂载停摆，与 §3.4 专设分 chunk 让锁的
  动机相悖；解压块仅归该 open 的句柄所有、无共享可变状态，让锁窗口
  内其他线程只做同 repository 的并发只读，libgit2 1.x 已保证安全
  （§3.4 让锁引用同一依据）。并发打开同一超限 blob 时各 open 并行
  解压各自的块，与 §7.7(a) 已声明的按 open 独立持有、内存峰值 =
  Σ 各 open 所持大小的口径自洽，不引入新峰值；single-flight 口径
  相应收窄为可缓存 blob 的 LRU 装载（锁内首个线程填充、其余命中
  复用，§7.6(e) 同步修订；该锁内串行口径后经 §7.9(a) 再修订为
  锁外解压+回锁双检复用，锁边界以本条为准、代价口径以 §7.9(a)
  为准）。该 open 自身的返回时间仍与全量解压
  同阶（数 GB → 数秒，调用方需预期），等待语义与 `commits` 首次
  open 同构，README 同步声明；§4 增可选参考断言“超限 blob open
  解压期间并发请求（根 readdir）不被长时间阻塞”（自管 runner、
  宽松阈值）；
  (b) 可观测性：每次超限 blob 全量解压记一条 verbose 日志（解压
  计数），集成测试断言“单次 open 顺序整读 → 解压计数 = 1”——
  直接断言“只解压一次”，补足 §7.7(d) 耗时比值断言只能抓 O(n²)
  回归、无法排除多次解压的盲区；man 页 `--blob-cache-size` 与
  `-v` 描述同步。

### 7.9 LRU 装载锁分段与尺寸读取路径（2026-09-24，review round 4/5 跟进）

- **Q19 可缓存装载锁分段与 blob 尺寸 header-only 读取（已决）**：
  (a) §7.8(a) 收窄后的“可缓存 blob 的 LRU 装载在单互斥下串行执行
  （锁内 single-flight）”再修订为**同构锁分段**：`--blob-cache-size`
  无人为上限（3.7），用户调大后接近上限的单个可缓存 blob 在锁内
  解压会让全挂载停摆数秒，与 Q18a 把超限解压移出锁的动机残余
  不对称；LRU 装载改为锁内 lookup（命中即返回）+ 双检标记、
  **锁外解压**、回锁复查后插入（他线程已插入同 oid 则丢弃自家
  块；插入时容量不足则不入池、调用方持私有块走绕过路径，与超
  限语义合流）；同一 miss 的并发首载可能瞬时重复解压（败者块
  即刻释放），不承诺严格 single-flight——瞬时内存上界 ∝ 并发
  装载数，远优于全挂载停摆；锁内仅剩 lookup 与逐出记账（微秒
  级）；README 同步声明该装载语义；§3.4/§3.5 正文与 §7.6(e)
  口径相应修订；§4 增可选参考断言“大容量挂载下大可缓存 blob
  首载期间根 readdir 不被长时间阻塞”；
  (b) blob 尺寸获取路径钉住为 **header-only 读取**
  （`git_odb_read_header`）：§3.2 的 `st_size` 与 §3.5 的超限准入
  判定一律经此获得，getattr/open 不为取 size 触发 blob 全量解压
  ——与 §3.3 已钉住的“getattr 不触发 revwalk”同构补全；§4 增
  “getattr 不产生解压日志”断言（`-v` 下 `ls -l` 含大 blob 目录、
  解压事件计数为 0）；man 页 File metadata 与 `--blob-cache-size`
  描述同步。

### 7.10 帮助文本措辞对齐（2026-09-24，review round 5/5 跟进）

- **Q20 -v 帮助文本解压事件范围措辞（已决）**：§3.7 CLI 帮助文本中
  `-v` 原仅写“超限 blob 全量解压事件”，未涵盖可缓存 blob 的 LRU
  装载解压，与 §3.5“缓存装载的解压同样记一条 verbose 日志”及
  man 页 `-v` 段“covering both oversized open-pin decompressions
  and cache-miss LRU loads”存在措辞范围差；改为“blob 全量解压
  事件（超限 open-pin 与缓存 miss 装载各一条）”，三处口径对齐，
  纯措辞修订、功能无影响。

### 7.11 mount(8) 助手协议核验勘误（2026-09-24，定稿后 review 跟进）

- **Q21 libmount 转交行为实测核验、重复键合并与 st_blocks（已决）**：
  以 util-linux 2.42.3 libmount 源码逐条核验助手协议后修订四项：
  (a) **`rw` 改为接受**：libmount exec 助手时无条件在 `-o` 串中预置
  `rw`（optlist.c 的 `mnt_optlist_strdup_optstr` 对
  MNT_OL_FLTR_HELPERS 恒 append "rw"，仅 MS_RDONLY 时转写为
  "ro"），而 Q10/Q13(c) 原定反向键 `rw` 报错退出 1——裸调用
  `mount -t gitfs <repo> <dir>`（不带 -o ro）必然 exit 1，与 §0/§3.7
  首推的默认调用形态自相矛盾；`rw` 改为接受为无操作并记 stderr
  警告（ro 为硬编码基线、安全不被稀释，ntfs-3g 同先例），`suid`/
  `dev` 维持报错退出 1；Q10/Q13(c) 的“rw 报错”口径就此作废；
  (b) **`-f` 即 fake、前台仅长选项**：libmount 的 exec_helper 在
  `--fake` 时向助手转交 `-f`（mount(8) EXTERNAL HELPERS 语法即
  [-sfnv]），Q13(a) 的“--fake 由 mount(8) 自身消化、-f 无歧义”
  不成立；且 `-r`/`-w` 从不以标志形式转交、而是并入 `-o` 串
  （`mnt_context_mount_setopt`：-r → 追加 ro、-w → 追加 rw），
  Q13(a) 的 -r/-w 标志映射行基于同一错误前提一并作废；短选项
  `-f` 实现为 fake（完整校验参数与全部选项后不挂载、退出 0；
  该口径经 §7.13(c)/Q23 钓含仓库校验：坏仓库 → 退出 2），
  前台改用长选项 `--foreground`，直连显式 `-r`/`-w` 按未知选项
  处理；
  (c) **自有键重复取“后者胜”**：util-linux ≥2.35 允许 `-o` 在 fstab
  选项之上增改（合并后同一键可出现两次），fstab
  `blob-cache-size=128` + CLI `-o blob-cache-size=256` 的正常覆盖
  流会命中 Q13(a) 的“同一键重复 → 退出 1”；自有键
  （blob-cache-size/tree-cache-size）改为按出现序后者胜（fstab
  → 命令行，-o 串内从左到右，与长选项按命令行先后交错计序），
  与 mount 惯例对齐并文档化；基线同义键与 `rw` 的重复本就是
  无操作/警告后无操作，不受影响；
  (d) **st_blocks/st_blksize/st_rdev 钉住**：libfuse 不自动按
  st_size 推导 st_blocks（缺省 0 会让 du 对全部文件报 0 块），钉
  为 ceil(st_size/512)（合成文件同口径）、st_blksize=4096、
  st_rdev=0（见 3.2）；
  §3.2/§3.7 正文与 man 页（INVOCATION/OPTIONS/File metadata/
  EXIT STATUS/EXAMPLES）同步修订；§4 新增“mount(8) exec 路径”集
  成用例三场景：默认调用（无 -o ro，断言 libmount 预置 rw 被接受
  且挂载成功）、`mount --fake` 转交 `-f`（校验通过、不挂载、退出
  0）、fstab+CLI 同键覆盖（后者胜，以 `.gitfs.json` 的 cache 字段
  断言生效值），另增 st_blocks 的 du 断言。

### 7.12 VFS 键到达口径、-N 转交、选项后置与退出码措辞（2026-09-24，定稿后 review round 2 跟进）

- **Q22 到达的 VFS 键集、-N 短选项、选项后置解析与退出码 0 措辞
  （已决）**：以本机 util-linux 2.42.3 真实 mount(8) exec 助手实测
  （安装临时 `/sbin/mount.gitfs-xyz` 助手捕获 argv）核验后修订
  四项：
  (a) **VFS 键到达口径勘误（中高）**：§3.7 原文"通用 VFS 键
  （exec/auto/user/async 等）由 mount(8) 翻译为挂载 syscall 标志、
  不会到达 gitfs"为事实错误——实测 libmount 仅滤除固定子集
  （auto/noauto/comment=/x-*/loop/offset=/sizelimit=/defaults 及
  传播键），noatime/relatime/strictatime/lazytime/diratime、sync/
  dirsync、exec/noexec、user/users/owner/group（且隐式附带
  noexec,nosuid,nodev）、_netdev、nofail、remount、uid=/gid=/
  umask= 均原样到达 `-o` 串（该到达名单与附带键措辞经
  §7.13(a)(d)/Q23 补全与精化：补 atime/nodiratime/iversion/
  silent/loud/mand/nomand/nofs 及 context= 族，并钉"截首个 =
  取键名"与规则优先的分诊；复经 §7.14(a)/Q24 补 symfollow/
  nosymfollow/nouser 入名单、锚点改"枚举优先 + 表轨兜底"双轨；
  再经 §7.15(a)/Q25 勘误 acl/quiet/showexec/bsdgroups 实为表外
  转发键、改入枚举轨并限定表轨仅认表内所列），
  按原口径全部命中"未知键 → libfuse
  拒绝 → exit 1"——`mount -t gitfs -o noatime repo dir`（常见
  习惯）必失败，fstab 含 `user`（非 root 挂载）或 `nofail`/
  `_netdev`（boot 常见）必失败，与 §7.11(a) 的 rw 问题同类、主
  集成路径仍被破坏。修订：到达的无关 VFS 键（noatime 族/sync
  族/exec 族/user 族/_netdev/nofail）接受并忽略、verbose 记一条
  （`rw`/ntfs-3g 先例；ro、atime≡mtime 语义下均无害）；`suid`/
  `dev` 维持退出 1（实测确实到达，规范可测）；`remount`、`uid=`/
  `gid=`/`umask=` 定为退出 1 并给专用错误文案（v0.1 无 remount/
  属主映射语义，静默忽略会掩盖用户意图）；§3.7 正文与 man 页
  INVOCATION/OPTIONS 同步，§4 的 mount(8) exec 路径用例由三场景
  扩至五场景（增 `-o noatime` 与 fstab `user` 两用例，另以直连
  调用断言 remount/uid= 的专用错误）；
  (b) **`-N` 转交补全（小）**：mount(8) 助手契约语法实为
  `[-sfnv] [-N namespace] [-o options]`（该语法引用经 §7.16(a)/Q26
  补全 man 页末位 `[-t type.subtype]` 子句），`--namespace` 时助手收到
  `-N <ns>`——§7.11(b) 的"只转交 -s/-f/-n/-v 与 -o"表述不完整，
  `-N` 原会按未知选项 exit 1；修订为与 `-n`/`-s` 同列容忍忽略
  （本沙箱无法实测 `-N` 转交——mount(8) 在 exec 前拒绝切
  namespace——但 man 页契约明载）；
  (c) **选项后置解析（小）**：实测助手 argv 为 `<src> <dir> -f -o
  rw`——选项出现在位置参数之后，§3.7 用法行原仅示选项在前；
  注明解析须容忍选项后置（GNU getopt 置换），否则主路径直接破；
  (d) **退出码 0 措辞勘误**：§3.7 与 man 页 EXIT STATUS 原"0 正常
  卸载"（"Normal unmount"）对 mount 助手不成立——mount(8) 传播的
  exit 0 语义是"挂载成功（守护进程化后父进程退出）"，卸载由
  umount(8)/fusermount3 完成、不经本程序；改为"0 挂载成功（或
  前台优雅终止）/fake 通过均 0"。

### 7.13 到达键名单补全与分诊规则、sloppy/fake 口径、user 族措辞（2026-09-24，定稿后 review round 3 跟进）

- **Q23 到达 VFS 键名单补全与分类规则、-s/-f 口径与 user 族
  措辞（已决）**：以本机 util-linux 2.42.3 真实 mount(8) exec
  助手复测 Q22a 名单后再修订四项：
  (a) **名单补全、键名匹配与分类规则（中）**：实测再漏键——
  `-o noatime,nodiratime`（经典 fstab 组合）两条均原样到达，而
  Q22a 名单只收 diratime 却漏 nodiratime（也未收 atime，自相
  矛盾）；iversion/silent/loud/mand/nomand/nofs 实测均到达、
  均不在名单；SELinux 标签键 context=/fscontext=/defcontext=/
  rootcontext=（值含冒号）实测到达、原口径未定；user=alice、
  nofail=1 实测带值到达、名单匹配未钉键名截取规则。修订：忽略
  名单补入 atime/nodiratime/iversion/silent/loud/mand/nomand/
  nofs；context= 族与 uid= 同理定为专用错误退出 1（SELinux 环境
  以 context= 挂载是常规操作，静默忽略安全标签意图不可接受）；
  名单匹配钉为"截首个 = 取键名"（带值形态按键名归入，值不再
  校验）；并增"到达键分诊规则"——mount(8) 通用 VFS 选项表中对
  gitfs 无操作的键接受并忽略、有语义/安全影响者专用错误退出 1、
  表外键透传 libfuse，规则优先于枚举，防 util-linux 演进再漏
  （连续三轮 review 均发现枚举漏键）；§3.7 正文与帮助文本、man
  页 INVOCATION/OPTIONS 同步，§4 的 mount(8) exec 路径用例由
  五场景扩至七场景（增 `-o noatime,nodiratime` 组合与
  `user=alice` 键值形态两用例，另以直连调用断言 remount/uid=/
  context= 的专用错误；名单与"表内/表外"单轨锚点复经
  §7.14(a)/Q24 勘误——补 symfollow/nosymfollow/nouser 入名单、
  锚点改双轨并加演进条款；§7.15(a)/Q25 再勘误四键表内归属）；
  (b) **-s 不实现 sloppy 放宽（小）**：mount(8) 文档语义为"忽略
  文件系统不支持的挂载选项"，但静默吞掉拼写错误的自有键会掩盖
  配置错误；显式文档化 `-s` 仅标志本身被容忍、不放松未知 `-o`
  键拒绝（§3.7 帮助文本与 man 页 OPTIONS 各一句话闭环，Q23b）；
  (c) **-f fake 钉含仓库校验（小）**：fake 的"完整校验参数"明确
  含仓库可读性（坏仓库 → 退出 2），仅跳过挂载与守护进程化——
  否则 `mount --fake` 会给 boot 时才失败的 fstab 条目开绿灯
  （§3.7 帮助文本与退出码、man 页 -f 与 §4 场景 2 同步，Q23c）；
  (d) **user 族附带键措辞精化**：实测 `user`/`users` 隐式附带
  noexec,nosuid,nodev 全三项，`owner`/`group` 仅附带 nosuid,
  nodev，`user=<name>` 不附带任何键——三者对硬编码基线均为无
  操作、行为不受影响，但措辞改为实测口径，§4 场景 5 对
  owner/group 不得写 noexec 存在性断言（§3.7 正文与帮助文本、
  man 页 INVOCATION 同步，Q23d）。

### 7.14 分诊规则双轨锚点、rw/ro 折叠钉住与 man 页 argv 序（2026-09-24，定稿后 review round 4 跟进）

- **Q24 到达键分诊锚点双轨化、-o rw,ro 折叠与 man 页 argv 展示
  （已决）**：round 4 以本机 util-linux 2.42.3 真实 mount(8) exec
  助手复测发现 §7.13(a) 的规则锚点（mount(8) man
  "Filesystem-independent mount options" 表）与枚举快照不自洽且
  自身会漏，修订三项：
  (a) **锚点改双轨 + 名单补全（中）**：`nofs` 在枚举接受名单内、
  却不在 util-linux 2.42.3 man 表内——按 §7.13(a) 规则(3) 落
  "表外 → 透传 libfuse → 退出 1"，与枚举的"接受并忽略"直接矛
  盾；`symfollow`（util-linux 2.41+ 新 VFS 键，实测原样到达、对
  gitfs 为无操作）不在表内亦不在枚举内——按规则退出 1，而其逆
  键 `nosymfollow`（表内）被接受，§7.13(a)"防 util-linux 演进
  再漏"的目标恰被 symfollow 这一演进键击穿；枚举自称 2.42.3
  实测快照却漏收实测到达且表内的 `nouser`/`nosymfollow`。修订：
  `symfollow`/`nosymfollow`/`nouser` 补入忽略名单（`acl`/
  `quiet`/`showexec`/`bsdgroups` 等其余实测到达的表内无操作键由
  表轨兜底、不逐一枚举——该括注的"表内"归属经 §7.15(a)/Q25
  勘误：四键实测均不在 2.42.3 man 表内，改入枚举轨）；分诊规则
  锚点改为**"枚举名单优先 +
  表内无操作键兜底"双轨**（枚举轨覆盖表外键 nofs/symfollow，
  表轨覆盖未枚举的表内键），并为表外新到达键加**演进条款**：
  新 util-linux 版本发布时按"是否对 gitfs 无操作"人工分诊入
  枚举名单，写入维护核对单（diff man 表与 libmount 转发键集 →
  分诊 → 更新枚举名单与 §4 用例）；§3.7 正文与帮助文本、
  man 页 OPTIONS/INVOCATION 同步，§4 的 mount(8) exec 路径用例
  由七场景扩至九场景（增 `-o symfollow` 与 `-o nouser` 到达
  用例）；
  (b) **man 页 INVOCATION argv 展示自相矛盾（小）**：同段先记
  实测"选项在位置参数之后"，后又写 invokes `mount.gitfs -o rw
  repo dir`（选项前置形态）——改为 `mount.gitfs repo dir -o rw`，
  与实测 argv 形态一致；
  (c) **-o rw,ro 同串折叠钉住（小）**：实测 libmount 将同串
  rw/ro 折叠为单个键（后者胜，`rw,ro` → 单个 `ro`），助手不会
  从 mount(8) 同时收到 rw 与 ro、无须自行解析该组合——fstab
  `ro` + CLI `-o rw` 的覆盖流同样在 libmount 层裁决为单个到达
  键、到达后均为无操作（`rw` 记警告），挂载恒为 ro（直连调用
  纵然出现 rw,ro 同串，两键亦各自为无操作）；§3.7 与 man 页
  INVOCATION 各以一段钉住。

### 7.15 表轨锚点限定、维护核对单落点、直连滤除键口径与带值 ro/rw 注记（2026-09-24，定稿后 review round 5 跟进）

- **Q25 四键表内归属勘误、演进条款载体、直连不对称声明与带值
  ro/rw 形态注记（已决）**：round 5 以本机 util-linux 2.42.3 man
  页复核 Q24 前提（symfollow/nofs 确不在 FILESYSTEM-INDEPENDENT
  表、nouser/nosymfollow 在表内均属实）并修订四项：
  (a) **四键改入枚举轨、表轨限定表内键（中）**：`acl`/`quiet`/
  `showexec`/`bsdgroups` 被 Q24a 误标为"表内无操作键、由表轨兜
  底"——实测 2.42.3 man 的 FILESYSTEM-INDEPENDENT 表对四键零匹
  配（quiet/showexec 属 FILESYSTEM-SPECIFIC 节的 FAT 选项、acl
  属 ext/ntfs/overlay 等 fs-specific 节、bsdgroups 全 man 页零
  出现），它们实测到达是因为 libmount 把表外未知键当 fs-specific
  数据转发，而非在表内；按双轨规则(4)（两轨皆不属 → 透传
  libfuse → 退出 1）四键会被拒，与 §3.7 到达段括注、§3.7 帮助
  文本、§7.14(a)、man 页 INVOCATION/OPTIONS 五处"表轨兜底接受
  忽略"的断言直接自相矛盾——恰是 Q24a 要消除的锚点失配类问题、
  由 Q24 自己引入。修订：四键改入枚举轨（实测到达且对 gitfs 无
  操作），表轨措辞限定"仅认 Filesystem-independent 表所列"，
  五处同步修订；顺带点破表内否定键 noiversion/norelatime/
  nostrictatime/nolazytime 正是表轨覆盖的表内键（现规则恰好覆
  盖但未点破，防再犯）；Q10/§7.12(a)/§7.13(a)/§7.14(a) 勘误链
  同步补注；
  (b) **维护核对单落点（小）**：§7.14(a) 演进条款引用的"维护核
  对单"在 §4 工程树原无落点（docs/ 仅 filesystem-semantics.md
  与 mount.gitfs.8）——增 `docs/maintenance-checklist.md` 节点，
  作为"diff man 表与转发键集 → 分诊 → 更新枚举名单与用例"流
  程的载体（§4 树与 §3.7 演进条款同步指向）；
  (c) **直连调用滤除键口径（小）**：直连 `mount.gitfs … -o
  defaults` 等在 mount(8) 路径上被滤除的键（defaults/auto/
  noauto/comment=/x-*/loop/offset=/sizelimit=/传播键）原无口
  径——按规则(4) 落"两轨皆不属 → 透传 libfuse → 退出 1"，与
  mount(8) 路径（滤除、恒成功）不对称且未声明；显式钉住该不对
  称为设计意图（这些是 mount(8) 侧指令，直连路径无消费者，报
  错以暴露无效意图），§3.7 规则(4) 与帮助文本、man 页 OPTIONS
  各一句；
  (d) **带值 ro/rw 形态注记（微）**：util-linux 2.41+ 的
  `ro=vfs`/`rw=fs`（及 recursive 参数形态）已被"截首个 = 取键
  名"规则覆盖（截=后键名即 ro/rw，落入既有无操作路径：ro 冗余、
  rw 记警告），原未注记——§3.7 规则(1) 与帮助文本、man 页
  OPTIONS 各一句点破，实现者无须为带值形态写特殊解析。

### 7.16 定稿后收尾勘误（2026-09-24，定稿后 review round 6 跟进）

- **Q26 助手契约 `-t` 子句、合成分组目录元数据、仅含 HEAD 的远端
  命名空间与两处边界钉住（已决）**：round 6 复核发现四处收尾缺口，
  均为一句级钉住、无结构调整：
  (a) **助手契约 `-t` 子句（小）**：man mount(8) EXTERNAL HELPERS 节
  的助手契约完整语法为 `[-sfnv] [-N namespace] [-o options]
  [-t type.subtype] <src> <dir>`——末位 `-t` 子句（带点 fstype 由
  libmount 以 `-t <type.subtype>` 转交；实测带点 fstype 的助手 argv
  为 `<src> <dir> -o rw -t <type.subtype>`，无点则恒不出现）在
  §3.7 的契约引用中缺位；`gitfs` 无点、经 mount(8) 的正常路径永不
  触发，但 §3.7 自称对助手契约逐条核验穷尽，缺该子句即非穷尽。
  修订：§3.7 契约引用与帮助文本内联契约补全语法；钉住处理——收到
  `-t gitfs`（仅可能经直连调用出现）接受并记 verbose 一条（冗余
  自指），其余任何值（含 `gitfs.<x>` 带点形态——用户
  `mount -t gitfs.<x>` 时 libmount 确会 exec `mount.gitfs` 并转交该
  值）为参数错误退出 1（fstype 错配）；§7.12(b) 的契约语法引用
  补勘误链注记；man 页 INVOCATION 同步；
  (b) **合成分组/命名空间目录元数据（小）**：嵌套 ref 的分组前缀
  节点（`/branch/feature`、`/tag/v1.0`、`/remote/origin` 等）既非
  五个入口目录（§3.2/Q16d 钉 mtime = 挂载时刻）也非 tree 目录
  （committer time），§3.1 原"分组前缀节点与完整 ref 节点均为目录，
  getattr 语义一致"未钉 mode/nlink/mtime，实现者可能自取 committer
  time 或运行时时钟。钉住：`S_IFDIR|0755`、`nlink=2`、
  `st_mtime/ctime/atime` = 挂载时刻（与入口目录同口径、非 tree 目录
  不取 committer time）；§3.1/§3.2 钉住、§4 增一条断言、man 页
  File metadata 同步；
  (c) **仅含隐藏 HEAD 的远端命名空间（小）**：`refs/remotes/
  <remote>/` 下仅有隐藏的 HEAD 符号引用时，`/remote/<remote>` 的
  渲染（抑制 vs 空目录）原无口径。钉住：命名空间存在性以 refdb 为
  准，仍渲染为**空目录**、不抑制——零特判，与"枚举 refs/remotes/
  下实际存在的首层命名空间"的 refdb 直读语义一致，跟踪 ref 出现
  后由实时枚举自动填充；§3.1 与 man 页 remote/ 条目各一句；
  (d) **两处边界（微）**：① 目录 `st_size` 原无口径（含根 `/`）——
  钉为 4096（全部目录统一：根、入口、分组与 tree 目录，与
  `st_blksize` 同量级的惯例值）；② 超限判定原为严格 `>`——恰等于
  `--blob-cache-size` 的 blob 会尝试入池、为容纳它逐出整池其余条目
  而自身又几乎占满全池（等效清空整池），与"为单个超限对象清空整池
  引起缓存抖动"的绕过动机矛盾——边界钉为 `>=`（恰等于上限的 blob
  同走绕过路径）；§3.2/§3.3/§3.5 钉住、§4 超限用例补边界断言、
  man 页 File metadata/--blob-cache-size 同步。
