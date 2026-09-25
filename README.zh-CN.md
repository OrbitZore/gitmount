<div align="center">

# gitmount

**把 git 仓库挂载为只读文件系统。**

用普通的 `ls`、`cat`、`grep`、`diff` 浏览任意分支、tag 与 commit——
无需 checkout，无需 worktree，无需 archive。

[![CI](https://github.com/OrbitZore/gitmount/actions/workflows/ci.yml/badge.svg)](https://github.com/OrbitZore/gitmount/actions/workflows/ci.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPL%20v3-blue.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus)
![Platform](https://img.shields.io/badge/platform-Linux-fcc624?logo=linux)
![libgit2](https://img.shields.io/badge/libgit2-%E2%89%A51.4-ef3a24?logo=git)
![FUSE 3](https://img.shields.io/badge/FUSE-3-00599C)

[English](README.md) · **简体中文**

</div>

---

```console
$ sudo mount -t gitmount /srv/repos/linux.git /mnt/linux
$ ls -A /mnt/linux
.gitmount.json  HEAD  branch  commit  commits  remote  tag
$ ls /mnt/linux/tag
v5.4  v6.1  v6.6  v6.12  v6.13
$ grep -n '^VERSION\|^PATCHLEVEL' /mnt/linux/tag/v6.13/Makefile
1:VERSION = 6
2:PATCHLEVEL = 13
$ diff -r --brief /mnt/linux/tag/v6.12 /mnt/linux/tag/v6.13 | head -3
Files /mnt/linux/tag/v6.12/Makefile and /mnt/linux/tag/v6.13/Makefile differ
Only in /mnt/linux/tag/v6.13: .cargo
...
$ grep ^a1b2c3d /mnt/linux/commits          # 解析 oid 前缀
a1b2c3d4e5f6...  （完整 40 位 oid）
$ sudo umount /mnt/linux
```

## 为什么需要 gitmount？

跨版本比较文件通常意味着反复 `git worktree add` / `git archive`，或
者克隆两份仓库。gitmount 让本地仓库的**每一个** ref 与 commit 直接呈现
为普通目录树，剩下的交给已有工具：

- **任意对比任意。** `diff -r /mnt/tag/v6.12 /mnt/tag/v6.13`——分支、
  tag、远端跟踪分支、裸 commit 都只是目录。
- **构造即安全。** 挂载强制 `ro,nosuid,nodev,default_permissions`，
  所有写路径返回 `EROFS`；经由挂载点做的任何操作都不可能改动仓库。
- **普通工具即可。** 编辑器、`grep`、`find`、`tar`、`rsync`、IDE——
  消费侧不需要任何 git 知识。
- **自包含。** 单个 `mount(8)` 助手，基于
  [libgit2](https://libgit2.com) 与
  [libfuse3](https://github.com/libfuse/libfuse)；不产生 `git` 子进
  程，不访问网络。

> **关于命名**：本项目早期开发阶段曾短暂叫过“gitfs”。该名字属于
> [presslabs/gitfs](https://github.com/presslabs/gitfs)——另一个更早
> 的项目（Python、读写、云存储后端），因此更名为 **gitmount**。对比
> 见 [RFC](rfc/0000-gitmount.md)。

## 特性

- 完整 tree 语义：目录、普通/可执行文件、符号链接（含原始字节文件
  名）、子模块（空目录 + 说明文件）
- 全部 ref 命名空间：`branch/`、`tag/`（annotated tag 自动 peel）、
  `remote/`、`HEAD/`，以及按完整 oid 访问任意 commit 的 `commit/`
- ref 实时性：挂载后新建的分支/tag 无需重挂载即可见；已解析对象只
  要仍在对象库中就继续可访问
- 确定性枚举：所有目录一律按原始字节（memcmp）字典序列出——稳定、
  可复现、与 locale 无关
- 有承诺的缓存：blob LRU + libgit2 tree 缓存；超限 blob 每次 open
  **恰好解压一次**（测试断言、`-v` 可观测）
- 加固基线：不遵循 `refs/replace`、非 UTF-8 名按原始字节透传、手工
  构造的病态对象跳过并告警
- 一等公民的 `mount(8)` 集成：`mount -t gitmount`、`/etc/fstab` 条目、
  直连调用三种形态等价；附 man 手册

## 环境要求

| 依赖 | 版本 | Debian/Ubuntu | Fedora | Arch |
|---|---|---|---|---|
| C++ 编译器 | C++17 | `build-essential` | `gcc-c++` | `gcc` |
| CMake | ≥ 3.16 | `cmake` | `cmake` | `cmake` |
| pkg-config | — | `pkg-config` | `pkgconf` | `pkgconf` |
| [libgit2](https://libgit2.com) | ≥ 1.4 | `libgit2-dev` | `libgit2-devel` | `libgit2` |
| [libfuse3](https://github.com/libfuse/libfuse) | ≥ 3.10 | `libfuse3-dev` | `fuse3-devel` | `fuse3` |

单测经 CMake FetchContent 拉取
[Catch2 v3](https://github.com/catchorg/Catch2)（配置期需要网络），
也可用 `FETCHCONTENT_SOURCE_DIR_CATCH2` 指向本地副本。集成测试需要
`/dev/fuse` 与 `fusermount3`，缺失时自动跳过。

## 安装

从源码构建（暂无发布版本——见 [CHANGELOG.md](CHANGELOG.md)）：

```sh
git clone https://github.com/OrbitZore/gitmount
cd gitmount
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build             # 可选：单测 + 集成测试
sudo cmake --install build         # /usr/sbin/mount.gitmount + man8 手册
```

## 快速开始

三种等价的调用形态：

```sh
sudo mount -t gitmount /path/to/repo.git /mnt/gitmount     # 经 mount(8)
sudo mount /mnt/gitmount                                # 经 /etc/fstab 条目
sudo mount.gitmount /path/to/repo.git /mnt/gitmount        # 直连调用
```

`/etc/fstab` 示例（开机自动挂载、仓库缺失时不阻塞启动）：

```
/srv/repos/linux.git  /mnt/linux  gitmount  ro,noatime,nofail  0  0
```

用 `sudo umount /mnt/gitmount` 卸载（或 `fusermount3 -u`）。守护进程收
到 `SIGINT`/`SIGTERM` 也会优雅退出。只校验配置而不实际挂载（含仓库
可读性检查）用 `-f`：

```sh
mount.gitmount /srv/repos/linux.git /mnt/linux -f && echo 配置有效
```

## 使用

### 目录布局

```
/mnt/gitmount/
├── branch/      # 本地分支（嵌套名渲染为目录）
├── tag/         # tag（annotated 自动 peel 至 commit）
├── commit/      # 按完整 oid 访问任意 commit——恒不可枚举
├── remote/      # 远端跟踪 ref：<remote>/<branch>
├── HEAD/        # 当前 HEAD 的快照
├── commits      # 全部可达 commit oid，每行一个
└── .gitmount.json  # 挂载元信息（不可变快照）
```

### 日常操作

```sh
diff -r /mnt/tag/v6.12 /mnt/tag/v6.13              # 对比两个 tag
grep -rn "TODO" /mnt/branch/topic-branch/src       # 在分支中检索
grep ^a1b2c3d /mnt/commits                         # 解析 oid 前缀
rsync -a /mnt/commit/<oid>/ /tmp/snapshot/         # 物化某个 commit
tar -C /mnt/tag/v6.13 -czf v6.13.tgz .             # 打包某个 tag
```

`/commit/<oid>` 仅接受完整小写十六进制 oid（40 或 64 位）；`commit/`
本身按设计恒不可枚举——请 `grep` `commits` 文件。

### 选项

```
-o blob-cache-size=<MiB>    blob LRU 缓存上限（默认 64）
-o tree-cache-size=<MiB>    libgit2 tree/commit 缓存预算（默认 256）
--blob-cache-size <MiB>     等价 -o blob-cache-size=<MiB>（支持 = 连写）
--tree-cache-size <MiB>     等价 -o tree-cache-size=<MiB>
--foreground                前台运行（默认守护进程化）
-f                          fake：完整校验参数与仓库，不实际挂载
-v, --verbose               解析日志 + 每次 blob 解压一条日志
--version, --help
```

`-o` 中的 `rw` 被接受为无操作并记警告（`mount(8)` 会无条件预置它）。
无关 VFS 键（`noatime`、`nofail`、`user`……）接受并忽略。`suid`、
`dev`、`remount`、`uid=`、`gid=`、`umask=`、`context=` 族与
`subtype=` 一律拒绝——只读基线不可协商。其余键透传 libfuse（如
`kernel_cache`、`allow_other`）。

退出码：`0` 成功 · `1` 参数错误 · `2` 仓库不可读 · `3` 挂载失败。

## 文档

- [docs/filesystem-semantics.md](docs/filesystem-semantics.md)——面向
  用户的稳定语义契约
- [docs/mount.gitmount.8](docs/mount.gitmount.8)——man 手册（安装后
  `man 8 mount.gitmount`）
- [rfc/0000-gitmount.md](rfc/0000-gitmount.md)——规范性设计文档
- [docs/maintenance-checklist.md](docs/maintenance-checklist.md)——
  运维核对单与实测性能基线

## 性能

在 5000 文件的合成树上实测（详见
[维护核对单](docs/maintenance-checklist.md)）：

| 负载 | ext4 | gitmount |
|---|---|---|
| `find -type f`（元数据遍历） | 6 ms | 49 ms |
| open+read+close（4 层路径） | 5 µs | ~350 µs |
| 直接 blob 读取（libgit2，无 FUSE） | — | 4.1 µs/blob |

内容密集型负载随数据平面良好伸缩；海量小文件的元数据风暴受往返延迟
约束——元数据超时钉死为 0（ref 的移动与删除必须立即可见，正确性优
先）。超限 blob 每次 open 恰好解压一次。

## 需要知道的语义

设计文档钉住的尖锐边界（完整契约见
[docs/filesystem-semantics.md](docs/filesystem-semantics.md)）：

- **`/commits` 首次 open 会停顿**——首次 `open()` 触发一次全量
  revision walk。Linux 内核规模的仓库上第一次 `cat` 会阻塞数秒；
  此前的 `stat` 报告 size 0 且绝不触发生成。
- **超限 blob 在 open 时钉住**——达到/超过 blob 缓存上限的 blob 在
  `open()` 解压一次并驻留内存直到关闭。打开多 GB 的 blob 需要数秒；
  期间其他请求不被阻塞（解压在全局锁之外执行）。
- **建议挂载期间禁用 gc**——外部 `git gc`/`git prune` 可能使在途请
  求瞬时 `ENOENT`/`EIO`，gc 结束后自行恢复。长期挂载的仓库可设置
  `git config gc.auto 0`。
- **不遵循 `refs/replace`**——输出与 `git --no-replace-objects` 一致。
- **st_ino 由路径派生且挂载期永不回收**——每百万个不同触及路径约数
  十 MB 内存。

## 常见问题

**可以通过挂载点写入吗？** 不行——所有写路径返回 `EROFS`。gitmount 设
计上就是只读数据平面。

**挂载不受信仓库安全吗？** 符号链接目标由仓库控制、原样透传（与
`git checkout` 同语义），路径被 VFS 限制在挂载点内。不要以 root 身
份浏览不受信仓库。

**支持 sha256 仓库吗？** 支持——oid 全程为 64 位十六进制。

**为什么 `df` 显示 100% 已用？** 只读卷没有可写空间；`statfs` 把本
地对象库占用量报告为卷容量、空闲为零。alternates 指向的外部存储不
计入。

**有些文件名显示乱码？** tree 条目名按原始字节透传；非 UTF-8 名与
`git checkout` 后的表现完全一致——不转义、不改名。

**怎么匹配已挂载的实例？** `/proc/mounts` 中类型为 `fuse.gitmount`、
源为 `gitmount`：`findmnt -t fuse.gitmount`。

**之前不是叫 gitfs 吗？** 早期开发阶段是的。该名字已被
[presslabs/gitfs](https://github.com/presslabs/gitfs)（Python、读写、
云后端，另一个项目）占用，因此本项目更名为 **gitmount**。

## 参与贡献

贡献流程见 [CONTRIBUTING.md](CONTRIBUTING.md)（Conventional
Commits、测试必需、CI 启用 `-Werror`）。社区规范见
[CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)；安全问题按
[SECURITY.md](SECURITY.md) 处理；变更记录见
[CHANGELOG.md](CHANGELOG.md)。

## 致谢

构建于 [libgit2](https://libgit2.com)、
[libfuse](https://github.com/libfuse/libfuse) 与
[Catch2](https://github.com/catchorg/Catch2) 之上；行为对照
[util-linux](https://github.com/util-linux/util-linux) mount(8) 与
git 本体实测校验。设计经 39 轮评审打磨，过程记录于
[RFC](rfc/0000-gitmount.md)。

## 许可证

[GPL-3.0-or-later](LICENSE) © gitmount 作者
