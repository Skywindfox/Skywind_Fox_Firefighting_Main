# armhf-toolchain — 统一的 armhf 交叉编译环境（Docker）

**这是本项目**唯一**的 armhf 交叉编译环境。** 目的是消除"每人一套工具链、
产出同名但来源不明的二进制"这个问题。

---

## 为什么要有这个目录

在这之前，armhf 二进制可能来自三套互不相同的环境：

| 来源 | 编译器 | glibc | GUI 依赖 | sysroot 在哪 |
|---|---|---|---|---|
| 旧: Fedora + Zig | Zig 0.16 (clang 21) | Zig 内建 | X11 + **Xft** + fontconfig | `~/Code/armhf-sysroot`（仓库外） |
| 旧: Windows + Docker | Debian gcc | 镜像内 | X11 + freetype | Docker 镜像（仓库外） |
| 板上原生编译 | 板上 g++ | 板载 | 板载 | 不需要 |

三者编译器和 libc 都不同。产物名带 commit 次数（`FireFv0.<n>`），
同一个提交三次编译出来同名、不同提交不同名；
出问题时第一步就得花时间确认"这是谁编的、它要的库本板有没有"。

**现在统一到这里**：环境由本目录的 `Dockerfile` 定义，随仓库走，
任何人构建出来的工具链完全一致。

---

## 快速开始

```bash
# 一次性：构建镜像（首次约几分钟）
docker build -t firecontrol-armhf:bookworm armhf-toolchain/

# 构建项目
./armhf-toolchain/build-armhf.sh              # Linux / macOS
.\armhf-toolchain\build-armhf.ps1             # Windows (Docker Desktop)

# 产物在 build-armhf/，其中 TOOLCHAIN.txt 记录工具链指纹
```

本机有 podman 也能用 —— 脚本会自动在 `docker` / `podman` 之间选择：

```bash
podman build -t firecontrol-armhf:bookworm armhf-toolchain/
./armhf-toolchain/build-armhf.sh
```

---

## 文件说明

| 文件 | 作用 |
|---|---|
| `Dockerfile` | 交叉编译环境定义。基于 `debian:bookworm-slim`（与目标板同版本），装 Debian 官方交叉编译器 + armhf 版 X11/freetype |
| `toolchain-docker-armhf.cmake` | CMake 工具链文件（容器内使用）。与旧的 `cmake/toolchain-armhf.cmake` 区分开 |
| `build-armhf.sh` | Linux/macOS 构建脚本（docker 或 podman） |
| `build-armhf.ps1` | Windows 构建脚本（Docker Desktop） |
| `verify-on-board.sh` | **在目标板上**校验产物：架构、ABI、运行时依赖、中文字体、X server |

---

## 隔离设计（这是本目录的重点）

"配好环境"只是第一步，**防止三种产物混淆**才是关键。做法有三层：

### 1. 产物目录带工具链指纹

每次构建都会在 `build-armhf/TOOLCHAIN.txt` 写入：

```
image      = firecontrol-armhf:bookworm
base       = debian:bookworm-slim
compiler   = 12.2.0
triplet    = arm-linux-gnueabihf
cmake      = 3.25.1
built_at   = 2026-09-12T14:22:31Z
host_runtime = docker
host_arch    = x86_64
```

这个文件**随产物一起拷到板上**，所以板上也能查"这个二进制是谁编的"。

### 2. CMake 配置阶段校验并写入指纹

`CMakeLists.txt` 在 configure 时把工具链信息写入 `TOOLCHAIN.txt`；
如果目标目录里已有**不同**工具的指纹，会给出警告而不是静默混用。

### 3. 上板前用 `verify-on-board.sh` 校验

```bash
./armhf-toolchain/verify-on-board.sh fbi@<板子IP>
```

它会检查：

- 32 位 ELF / ARM 架构 / **硬浮点 ABI**（板子是 `arm-linux-gnueabihf`）
- **每个运行时依赖在板上都存在**（`readelf -d` 的 NEEDED 逐个查）—— 缺一个就报错
- 中文字体（文泉驿正黑）在不在 —— 不在则界面汉字会变方框
- X server 在跑（`/tmp/.X11-unix/X0`）
- 打印该产物的工具链指纹

**产物与本板匹配才建议运行**，否则先换工具链重编。

---

## 环境内容（Dockerfile 做了什么）

```
基础      debian:bookworm-slim          ← 与目标板 Debian 12 同版本, glibc 不错配
编译器    gcc-arm-linux-gnueabihf       ← Debian 官方源, 版本随仓库
          g++-arm-linux-gnueabihf
          binutils-arm-linux-gnueabihf
构建工具  cmake / make / ninja-build / pkg-config
armhf 库  libx11-dev:armhf              ← LVGL 的 X11 显示/输入后端
          libfreetype-dev:armhf         ← 中文字形
          libgpiod-dev:armhf            ← GPIO 输入读取 (gpio_read_value, v1 API)
```

**依赖发现机制**：Debian multiarch 下，armhf 库装在
`/usr/lib/arm-linux-gnueabihf/`，与宿主的 `x86_64-linux-gnu` 天然隔离；
交叉编译器默认就搜多架构目录。所以不需要像手工 sysroot 那样维护
include/lib 路径列表 —— 版本永远和 Debian 仓库一致，不会漂移。

**为什么是 amd64 镜像**：这是**交叉编译**环境，跑在开发机上，产出 armhf 二进制。
不需要 binfmt/qemu 模拟，构建也快。

---

## 镜像内自检

`Dockerfile` 在构建阶段就编译一个含 X11 + FreeType + libgpiod 的探针程序，
并检查产物是 ARM 且带硬浮点 ABI。**自检失败会让 `docker build` 直接失败**，
不会留下一个"看起来能用实则残缺"的镜像。

探针里刻意调用了 libgpiod 的 **v1** API（`gpiod_chip_open_by_label`）——项目代码用的
就是 v1。哪天基础镜像升到 libgpiod v2（那些符号被删掉了），**这一步就会失败**，
而不是等编项目时报一堆"未声明标识符"。

所以：**镜像构建成功 = 工具链可用**。

---

## ✅ 实测状态（已跑通，非推断）

| 环节 | 结果 |
|---|---|
| 环境镜像构建 | ✅ `debian:bookworm-slim` + gcc-arm-linux-gnueabihf 12 + cmake 3.25.1 |
| 镜像自检 1/2 | ✅ 能交叉编译 X11 + FreeType + libgpiod(v1) 的 armhf 程序（ELF32/ARM/hard-float） |
| 镜像自检 2/2 | ✅ pkg-config 能查到 armhf 的 x11/freetype2/libgpiod（版本 1.6.3） |
| 容器内 cmake 配置 | ✅ 编译器 `/usr/bin/arm-linux-gnueabihf-g++`，libs `X11;freetype`，GPIO `libgpiod 1.6.3 (v1)` |
| 容器内完整编译 | ✅ LVGL 全量 + `FireFv0.<n>` / `FireFv0.<n>-console` 链接成功，318 个编译单元 0 warning |
| 产物 | `FireFv0.<n>` **407,936 字节**，ELF32 ARM hard-float，依赖 `libX11`/`libfreetype`/`libgpiod`/`libstdc++`/`libgcc_s`/`libc` |
| `verify-on-board.sh` 检查逻辑 | ✅ 32 位 / ARM / 硬浮点 / 6 个依赖 全部通过 |

网络访问不了 `docker.io` 时，用镜像站即可（已验证可用）：

```bash
BASE_IMAGE=docker.m.daocloud.io/library/debian:bookworm-slim \
    ./armhf-toolchain/build-armhf.sh
```

```powershell
.\armhf-toolchain\build-armhf.ps1 -BaseImage docker.m.daocloud.io/library/debian:bookworm-slim
```

---

## 构建方式：为什么用 COPY 而不是挂载

常规做法是 `docker run -v <仓库>:/work` 把源码挂进容器。但实测在部分环境
（podman + 受限沙箱）挂进来的目录**只读**：

```
mkdir: cannot create directory '/work/build-armhf/CMakeFiles': Permission denied
```

与 SELinux / 目录属主**无关** —— 干净的 `user_home_t` 目录同样写不了。

所以本目录改用 `Dockerfile.build-armhf` + `COPY`：源码复制进镜像层编译，
产物再用 `podman cp` 取回宿主，**全程不依赖 bind mount**。

**代价**：没有增量编译，每次全量重编 LVGL（本机 4 线程约 20 秒）。
**收益**：任何环境都能构建。

---

## ⚠️ 其他注意事项

- **Windows 上需要 Docker Desktop 处于运行状态**，脚本会检测并给出提示。
- **Fedora 上不需要 `docker`** —— 装 `podman` 即可，脚本会自动选择运行时。
- **SELinux**：脚本用 `--security-opt label=disable` 而非 `:Z`。后者会**永久改写
  宿主目录的 SELinux 标签**，且 `restorecon` 会以 "customized by admin" 为由
  拒绝恢复（实测踩到过）。前者只对本容器关闭隔离，不动宿主标签。
- **已在 Windows + Docker Desktop 上实测通过**（2026-09-12，FBI-0537）。
  首次实测暴露的几个"必然失败"问题及修法：

  | 问题 | 修法 |
  |---|---|
  | 自检缺 freetype 的 include 路径 | 改用 `pkg-config --cflags --libs x11 freetype2`，不再手写 `-I` |
  | 自检用了 slim 镜像里没有的 `file` | 改用 `readelf`（binutils 自带） |
  | `deb.debian.org` 间歇性断连（下到一半断） | 默认源换成清华 TUNA，见下 |
  | `build-armhf.ps1` 缺 UTF-8 BOM，PS 5.1 按 ANSI 读导致中文语法崩溃 | 加 BOM |
  | `$ErrorActionPreference='Stop'` 把原生命令的 stderr 当终止错误 | 改 `Continue`，成败看 `$LASTEXITCODE` |
  | `Set-Content -Encoding UTF8` 在 PS 5.1 写出 BOM+CRLF | 改 `[System.IO.File]::WriteAllText` 显式无 BOM + LF |

- **默认软件源是清华 TUNA** —— `deb.debian.org` 在部分网络下会"前 10 MB 能下、
  后面直接连不上"，导致镜像构建随机失败。要用官方源：

  ```bash
  docker build --build-arg APT_MIRROR=deb.debian.org \
               -t firecontrol-armhf:bookworm armhf-toolchain/
  ```

- **Windows 检出注意**：仓库根的 `.gitattributes` 已把 `*.sh` / `Dockerfile` 锁成 LF。
  Git for Windows 默认 `core.autocrlf=true` 会把它们检出成 CRLF，那样的 `.sh` 拿到
  板子或容器里执行会报 `bad interpreter: No such file or directory`。
  **已经用 CRLF 检出的工作区需要重新检出**才会变成 LF：
  `git rm --cached -r . && git reset --hard`（或直接重新 clone）。
- 旧的 `cmake/toolchain-armhf.cmake`（Zig 版，编译**纯逻辑层与控制台程序**）
  **已废弃**：`build.sh` 里的 Zig 路径（`armhf` / `verify` 模式）已删除，
  现在没有任何脚本引用这个工具链文件。它留在仓库里只作历史参考
  （相关的坑记录在 [../WARNING.md](../WARNING.md) 的 B 节）。
  交叉编译**一律**走本目录的 Docker 环境。
