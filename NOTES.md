# NOTES.md — FireControlApp 详细笔记

> [README.md](README.md) 只讲项目的目的、意义与要做的事。**这一份放细节**：
> 环境清单、构建与部署步骤、界面布局与尺寸、配色、实现要点、可选依赖、实测数据、待办。
> 踩坑与已知问题仍然在 [WARNING.md](WARNING.md)；真机验收在 [VERIFY-RUNBOOK.md](VERIFY-RUNBOOK.md)；
> 未决取舍与协作约定在 [workflow.md](workflow.md)。

---

## 目录结构

```
FireControlApp/
├── include/            对外头文件 —— 库与程序都只从这里 include
│   ├── greeting.h        纯逻辑层
│   ├── hardware_path.h   板上 sysfs 路径常量 (LED / 蜂鸣器)
│   ├── json_frame.h      start/end 帧解析 (流式、可处理半包)
│   ├── json_status.h     JSON 状态 (data / smoke)
│   ├── start.h           骨架: 四个阶段 + 程序级全局量
│   └── usetools.h        文件读写 / GPIO 输入
├── src/
│   ├── library/        静态库实现 → libfirecontrol.a
│   │   ├── greeting.cpp  纯逻辑
│   │   ├── json_frame.cpp start/end 帧解析
│   │   ├── json_status.cpp cJSON 解析 {"data":..,"smoke":..}
│   │   ├── start.cpp     唯一主循环 + 板载 LED/蜂鸣器 + 外部蜂鸣器线程
│   │   └── usetools.cpp  sysfs / libgpiod
│   ├── source/         程序入口
│   │   └── main.cpp      收参数 → cpp_start()
│   └── appgui/         界面实现
│       ├── gui.cpp       LVGL 界面 → deffire-gui-dev
│       └── gui_stub.cpp  无界面版 → deffire-dev
├── tests/              单元测试 (ctest)
├── third_party/cjson/  cJSON v1.7.18 源码 (MIT)
├── third_party/lvgl/   LVGL v9.2.3 源码 (已裁剪)
├── armhf-toolchain/    交叉编译唯一入口
└── CMakeLists.txt / lv_conf.h / build.sh / *.md
```

三个子目录与 CMake 的目标一一对应：

| 目录 | 目标 |
|---|---|
| `include/` + `src/library/` | `libfirecontrol.a` |
| `src/source/main.cpp` + `src/appgui/gui.cpp` | `deffire-gui-dev` |
| `src/source/main.cpp` + `src/appgui/gui_stub.cpp` | `deffire-dev` |

新加文件时**先想清楚它属于库还是程序**，再放进 `src/` 下对应目录并在 CMake 里登记。

---

## 1. 开发环境

| 项目 | 值 |
|---|---|
| 开发机 | Fedora 44 (Workstation) / Windows + Docker Desktop |
| 编译工具 | gcc 16.2.1、cmake 4.3.0、ninja 1.13.2、gdb 17.2 |
| GUI 依赖 | X11 + FreeType（容器内为 armhf 版） |
| 桌面会话 | LXDE / X11（开发机与板上一致；Windows 侧只构建，不跑 GUI） |
| 容器运行时 | `podman`（Fedora）或 `docker`（Windows），构建脚本自动选 |

原生构建需要：

```bash
# Fedora
sudo dnf install gcc gcc-c++ glibc-devel cmake ninja-build make gdb \
                 libX11-devel freetype-devel pkgconf-pkg-config
```

---

## 2. 交叉编译环境

**统一入口：`armhf-toolchain/`**，环境由仓库里的 `Dockerfile` 定义，随代码走。

```bash
cd FireControlApp
./armhf-toolchain/build-armhf.sh          # Linux / macOS（docker 或 podman 自动选）
.\armhf-toolchain\build-armhf.ps1         # Windows (Docker Desktop)
```

镜像站（`docker.io` 不可达时）：

```bash
BASE_IMAGE=docker.m.daocloud.io/library/debian:bookworm-slim \
    ./armhf-toolchain/build-armhf.sh
```

```powershell
.\armhf-toolchain\build-armhf.ps1 -BaseImage docker.m.daocloud.io/library/debian:bookworm-slim
```

| 文件 | 作用 |
|---|---|
| `Dockerfile` | 环境定义：`debian:bookworm-slim`（与板子同版本）+ Debian 官方交叉编译器 + armhf 版 X11/freetype |
| `Dockerfile.build-armhf` | 构建期用：源码 COPY 进镜像层编译 |
| `toolchain-docker-armhf.cmake` | 容器内用的 CMake 工具链文件 |
| `build-armhf.sh` / `.ps1` | 两个平台的构建入口 |
| `verify-on-board.sh` | 上板前校验产物（架构 / ABI / 运行时依赖 / 字体 / X） |
| `README.md` | 工具链详细说明 |

**产物隔离**：每次构建写入 `build-armhf/TOOLCHAIN.txt`（工具链指纹），随产物拷到板上，
出问题时先看它确认「这个二进制是谁编的」。

### 2.1 旧路径：Zig（已废弃）

`cmake/toolchain-armhf.cmake` + `~/.local/bin/arm-linux-*`（Zig 0.16 包装脚本）曾用于
交叉编译**纯逻辑层与控制台程序**（没有 X11/freetype，编不了 GUI）。

统一到 `armhf-toolchain/` 的 Docker 环境后，`build.sh` 已删除对应的
`armhf` / `verify` 模式，这条路径不再有入口。工具链文件本身还留在仓库里，
但已经没有脚本引用它。

踩过的坑记录在 [WARNING.md](WARNING.md) 的 B 节 —— 那不是废纸，是换来的经验。
---

## 3. 目标板环境

| 项目 | 值 |
|---|---|
| 硬件 | 正点原子 ATK-DLMP135（STM32MP135） |
| CPU | Cortex-A7 单核 1 GHz（ARMv7-A，VFPv3 + NEON） |
| 系统 | Debian 12 bookworm armv7l，内核 5.15.24 |
| 内存 | **437 MiB**（这是选型硬约束） |
| 屏幕 | 1024×600（正点原子 7 寸 RGB LCD，型号 7016） |
| 触摸 | Goodix 电容触摸屏，I2C-1 |
| 桌面 | Xorg `:0` + LXDE / **Openbox**（有窗口管理器，见下） |

> 桌面环境由板上 `neofetch` 实测确认：`DE: LXDE` / `WM: Openbox` / `Resolution: 1024x600`。

**有窗口管理器的后果**：`lv_x11_window_create()` 建的窗口会被 Openbox 接管 ——
开发机上实测窗口被加上装饰，报出来的尺寸是 1074x687 而不是请求的 1024x600。
开发机与板上现在都是 LXDE/X11（板上 WM 为 Openbox）。

**2026-10-18 决定：不自己实现真全屏。** 全屏交给 DE —— 应用侧按 EWMH 发送
`_NET_WM_STATE_FULLSCREEN` 请求（或在 LXDE/Openbox 里配窗口规则），是否铺满由 WM
决定，不在 GUI 里手工算坐标、也不造无边框窗口。代码里目前还没发这个请求，
详见 [workflow.md](workflow.md) §2.2 与 [WARNING.md](WARNING.md) C-7。

**板上需要装的**：

```bash
sudo apt install g++ libx11-dev libfreetype-dev cmake     # 若要在板上原生编译
sudo apt install libgpiod-dev                             # 同上, 且要编 GPIO 读取时
sudo apt install xinput x11-utils x11-apps                # 排查触摸/显示时才需要
sudo apt install gpiod                                    # 排查 GPIO 时才需要 (gpiodetect / gpioinfo)
```

**运行时依赖**：
`libX11.so.6` / `libfreetype.so.6`（X 桌面自带），中文字体 `fonts-wqy-zenhei`，
以及 **`libgpiod2`**（`sudo apt install libgpiod2`）。

> ⚠ `libgpiod2` 是**硬依赖**：交叉编译出来的 `deffire-gui-dev` 链着
> `libgpiod.so.2`（`gpio_read_value()` 用它）。板上没有这个库时**整个界面起不来**
> ——动态链接器在 `main()` 之前就会报 `error while loading shared libraries`，
> 不是"传感器功能不可用"那么局部。`verify-on-board.sh` 会把缺失的库逐个列出来。
> 不想要这个依赖就 `WITH_GPIOD=OFF` 重新构建（见 §6）。

**内存约束**是选型依据：裸 Xlib 约 2–4 MiB，GTK4 空程序要 40–80 MiB。
当前用的 LVGL + FreeType 实测常驻约 **136 MB / 437 MB**。

---

## 4. 部署到板子

项目是**开发机交叉编译、产物拷到板上跑**，板上不编译（性能不够）。所以没有
"板上 `git pull`"这条路，一切都从开发机推。

**板上统一用 `~/Desktop`**：所有产物和脚本都放那里。

```bash
./armhf-toolchain/verify-on-board.sh fbi@<板子IP>        # 上板前先校验产物与本板匹配
./armhf-toolchain/deploy-to-board.sh fbi@<板子IP>        # 传输产物 + 权限脚本到 ~/Desktop
./armhf-toolchain/deploy-to-board.sh --run fbi@<板子IP>  # 传完直接启动
```

必须显式给 `用户@IP`：开发机上通常解析不了板子的短主机名。板子的普通用户是 `fbi`。

**首次部署还要在板上做一次权限配置**（否则界面里点 LED / 蜂鸣器没反应，
`gpiodetect` 之类也会 `Permission denied`）：

```bash
ssh fbi@<板子IP>
sudo bash ~/Desktop/setup-board-permissions.sh fbi
sudo reboot
```

原因见 [WARNING.md](WARNING.md) 的 D-8 / D-9 两节 —— 这是**两个独立的坑**：
`/sys/class/leds/*/brightness` 默认是 root 只写，普通用户写的失败被**静默吞掉**；
`/dev/gpiochip*` 默认是 root 独占的字符设备，普通用户**明确报 `Permission denied`**。
两者子系统不同（`leds` / `gpio`），需要两条 udev 规则。
---

## 5. GUI 布局与交互

界面是**深色工业简约风**的三段式面板：顶部标题栏 / 中间内容区 / 底部操作栏。

| 页面 | 内容区 | 底栏 |
|---|---|---|
| 首页 | `halloworld` / `首页` / JSON 状态 | **12 个信息源按钮**（2 行 × 6 列） |
| 信息源 N | **独立全屏覆盖层**：顶栏(品牌/首页/退出) + 左侧信息源列表 + 中间接收/报文卡片 + 右侧信息源设置 | 覆盖层内操作，不用全局底栏 |
| 设置 | `halloworld` / `设置` / JSON 状态 | LED 调试 / 蜂鸣器 调试 / 外部无源蜂鸣器 / 返回 |
| LED / 蜂鸣器 / 外部蜂鸣器 | `halloworld` / 页名 / JSON 状态 | 开 / 关 / 心跳 / 返回（外部蜂鸣器没有心跳） |

顶栏右侧是 **【设置】+【退出】** 两个小按钮；设置页收纳了原先放在首页的三个本地测试入口，
首页因此腾出位置给 12 个信息源。

**信息源页不是普通的三段式页，而是一个全屏覆盖层**（参考样图，深蓝灰底 + 琥珀描边）：
顶栏是「火灾控制终端 / 运行中 + 首页 + 退出应用」；左侧是 12 个信息源按钮（可滚动、
当前项描边加粗）；中间是「接收：烟雾/火焰/温度/蜂鸣器/LED」和「模块状态 / 收到的报文 /
预留」两张卡片；右侧是「信息源设置」：GPIO 接口 / 线号下拉框、占位按钮、应用设置按钮。
全局顶栏/底栏会被这个覆盖层盖住，点覆盖层顶栏的【首页】回到普通首页。

普通页是**分页**的：顶栏（品牌 / 状态灯 / 设置 / 退出）所有页完全一致，切页只重建底栏按钮。
按钮表在 `gui.cpp` 的 `kHomeButtons`（12 个信息源）/ `kSettingsButtons`（本地测试）/
`kLedButtons` / `kBuzzerButtons` / `kOutBuzzerButtons`（页表是 `kPages`）。
**"每行放几个"是每页各自的参数**（`PageSpec::columns`：首页 6 列 → 2 行 × 6 列，
设置/调试页 4 列 → 一行放完）；行数按 `count / columns` 自动算，末行不满时补透明占位，
保证各行按钮等宽。数组长度用 `countOf()` 取，不要手写数字。
`Page::Sensor` 是一类页，具体编号在 `g_sensor_index`；它显示覆盖层，不走全局底栏。

| 操作 | 效果 |
|---|---|
| 点顶栏右侧 **【设置】** | 进设置页（所有本地测试入口） |
| 点顶栏右侧 **【退出】** | 退出程序（所有页都一样） |
| 首页点 **【信息源 1】…【信息源 12】** | 进对应信息源接口页（全屏覆盖层） |
| 接口页左侧 **【信息源 N】** | 切换模块，不离开接口页 |
| 接口页顶栏 **【首页】** | 回普通首页（关闭覆盖层） |
| 设置页点 **【LED 调试】/【蜂鸣器 调试】/【外部无源蜂鸣器】** | 进对应调试页 |
| 调试页里点 **开 / 关 / 心跳** | 控制板载 `sys-led` / `beep`。板上需要先配权限（见 §4） |
| 外部无源蜂鸣器页里点 **开 / 关** | 控制 PA6（GPIOA:6）上的无源蜂鸣器（**2 kHz 方波**，由专用线程发；可用 `FIRECONTROL_BUZZER_HZ` 在板上试别的频率）。该页**没有心跳** —— 它不归内核 leds-gpio 管，没有 `trigger` 文件 |
| 调试页末位 **【返回】** | 回首页；接口页用顶栏【首页】回首页 |
| 按 `q` / `Esc` | 退出程序（所有页都一样） |

改界面前先读 [workflow.md](workflow.md) §3 的约定（`Action::Nothing` 必须留在枚举
第一位、`switch` 不写 `default`、切页只置标志、别改控制台那行的格式）。

打印坐标是故意留的：回归时点一下就能核对触摸命中/偏移（见 VERIFY-RUNBOOK §4.2），
坐标落在按钮实际区域内、靠近中心即可。

**配色**（全部集中在 `gui.cpp` 顶部一段常量里，换风格只改那一段）：

| 用途 | 色值 | 说明 |
|---|---|---|
| 屏幕底 | `#121417` | 近黑，强光下不反光、夜间不刺眼 |
| 顶栏 / 底栏 | `#1A1D21` | 比底色略亮，把操作区分出来 |
| 按钮表面 | `#23272E` | 已实现的按钮 |
| 描边 / 分隔线 | `#2E343B` | 1px 走线 |
| 主文字 | `#E6E8EA` | 近白 |
| 次要文字 | `#8A9199` | 状态说明、未实现按钮 |
| 强调色（安全琥珀） | `#FFB020` | 品牌竖条、标题短线、**按下态描边** |
| 状态灯 | `#35C46A` | 运行中 |

信息源接口页另有一套更克制的深蓝黑配色：`kSensorBg #0E141B` / `kSensorPanel #161D26` /
`kSensorLine #2A3644`（细描边）/ `kSensorAccent #F2B544`（只用于选中、标题、主按钮）/
`kSensorText #E8EDF2` / `kSensorValue #FFFFFF` / `kSensorOk #4ADE80`（正常状态）。
不再给每个框都上橙色描边，只保留少量琥珀强调，整体更干净。

**实现上的几个点**：

- **三段式**由 `buildHeader` / `buildContent` / `buildFooter` 三个函数搭出来，
  容器一律透明无描边（`createPane`），顶栏底栏的 1px 分隔线用 `border_side`
  只画一条边（`createBar`）。
- **普通页布局全用 flex**：顶栏是 `SPACE_BETWEEN`（品牌 / 状态灯 / 右侧【设置】+【退出】），
  底栏每个按钮 `flex_grow=1` 自动等宽；间距 = 屏宽的 1%。换屏幕尺寸不用改坐标。
- **信息源页是独立全屏覆盖层**：`g_sensor_page` 是 screen 的最后一个子对象，只在
  `Page::Sensor` 时显示，盖住普通页的顶栏/底栏；`g_generic_content` 在它下面隐藏。
  布局：顶栏 10% 高、左侧 22%、右侧 24%、中间剩余；左侧按钮可滚动，当前模块描边加粗。
- **0 = 正常 / 1 = 异常**：这是传感器模块那边定义的，app 不做极性换算，也不提供
  "高有效/低有效"的选择。
- **右侧【应用设置】只保存选择，不读硬件**：现在没有"按信息源读 GPIO"的实现，点应用只打印
  `信息源 N 应用: GPIOx:y (仅保存选择, 未读硬件)` 并把模块状态改成
  "已选择 ...（等待数据接入）"，不会伪造数据。每个信息源各自保存一份选择
  （`g_sensor_cfg[12]`），默认值集中在 `kDefaultSensorConfig[12]`；现在全部是 `-1`
  （未配置），等实际 12 组默认映射提供后填进去，启动时就会显示"已选择"。
- **中间卡片**：上卡是"接收：" + 烟雾/火焰/温度/蜂鸣器/LED 五行，每行三列
  （名称 / 值 / 状态）；下卡是"模块状态 / 收到的报文 / 预留"。烟雾、火焰默认
  `0` / `正常`，温度 `--` / `待接入`，蜂鸣器、LED `--` / `规则待定`。
  收到完整帧后，报文行显示原始 `{"start":1}...{"end":1}`，长报文可滚动/换行。
- **界面配色**：普通页仍是近黑+安全琥珀；信息源覆盖层改用深蓝灰底 + 琥珀/橙描边
  （`kSensorBg` / `kSensorPanel` / `kSensorLine` / `kSensorText`），只作用于接口页。
- **下拉框细节**：列表是懒创建的，配色在 `LV_EVENT_READY` 里设；默认的
  `LV_SYMBOL_DOWN` 在 FreeType 字体里可能缺字，所以关掉 symbol，右侧单独放一个
  ASCII `v`；选中项高亮也关掉了，避免主题默认的蓝色和深色/琥珀色界面冲突。
- **按下反馈是即时的**：底色抬一档 + 描边点亮成强调色，同时 `lv_conf.h` 里把
  样式过渡动画关掉了（默认 80 ms），触摸要的就是"按下去立刻有回应"。
- **状态灯现在只表示"程序在跑"**（固定的绿灯 + "运行中"）。以后接上传感器/联动
  状态时，改 `buildHeader` 里的文字和灯色即可。
- **窗口尺寸**只在启动时算一次（`--windowed` 只有开发机预览时才用）。
  板上与开发机都是 LXDE/X11，窗口会被 WM 接管、加装饰 —— 按屏幕尺寸建窗口**不等于**
  真全屏。按 2026-10-18 的决定，全屏交给 DE（EWMH 请求/窗口规则），不在 GUI 里自己实现；
  见 [workflow.md](workflow.md) §2.2 与 WARNING.md C-7。
- **字体**：主标题 40 px、标题栏与按钮 18 px、状态文字 16 px；中文字体在启动时
  按候选表找一个字体**文件**（而不是字体名，避免 fontconfig 静默替换成不含汉字的字体）。
  候选表没命中时会**现扫** `/usr/share/fonts` 等目录（按 文泉驿 > Noto CJK > 思源 >
  Droid Sans Fallback > UMing 的优先级取最优）—— 发行版的包布局会变，硬编码单条路径
  迟早失效（Fedora 现在把 Noto CJK 装在 `google-noto-sans-cjk-vf-fonts/` 下）。
- **`q` / `Esc` 为什么能全局生效**：LVGL 的键盘输入只发给"输入组里当前被聚焦的
  对象"，组里没有聚焦对象时按键会被直接丢掉。所以启动时把**屏幕本身**加进默认
  组并聚焦它（按钮不进组，点按钮不会把焦点抢走）。
- **启动后 400 ms 内的点击会被忽略**：窗口刚映射时可能收到遗留的杂散点击，
  见 [WARNING.md C-6](WARNING.md#c-6-窗口映射瞬间可能收到杂散-buttonpress)。
- **窗口模式下会画一个跟随鼠标的小指针**（强调色）：LVGL 的 X11 后端会把 X 光标
  隐藏掉，板上是触摸屏不需要，但开发机预览时不画个指针就不知道鼠标在哪。

**注意**：**不支持"双击窗口退出"** —— 那是早期行为，已移除；原因见
[WARNING.md C-6](WARNING.md#c-6-窗口映射瞬间可能收到杂散-buttonpress)。

按钮在 1024×600 上的实际尺寸（按窗口比例算，换屏幕不用改代码）：

| 页面 | 按钮 | 位置 (x, y) | 尺寸 |
|---|---|---|---|
| 任意页 | 设置（顶栏右侧） | (842, 13) | 72×40 |
| 任意页 | 退出（顶栏右侧） | (922, 13) | 92×40 |
| 首页 第 1 行 | 信息源 1 / 2 / 3 / 4 / 5 / 6 | (10, 408) / (179, 408) / (348, 408) / (517, 408) / (686, 408) / (855, 408) | 159×78 |
| 首页 第 2 行 | 信息源 7 / 8 / 9 / 10 / 11 / 12 | (10, 512) / (179, 512) / (348, 512) / (517, 512) / (686, 512) / (855, 512) | 159×78 |
| 设置页 | LED 调试 / 蜂鸣器 调试 / 外部无源蜂鸣器 / 返回 | (10, 460) / (263, 460) / (516, 460) / (769, 460) | 243×78 |
| 调试页（LED / 蜂鸣器） | 开 / 关 / 心跳 / 返回 | (10, 460) / (263, 460) / (516, 460) / (769, 460) | 243×78 |
| 外部无源蜂鸣器页 | 外部蜂鸣器 开 / 关 / 返回 | (10, 460) / (348, 460) / (686, 460) | 328×78 |

- 底栏按钮高 **78 px**（屏高的 13%）。原来取 9%（54 px），实测**手感偏紧、不好按**，
  所以加高；代码里限制在 48–96 px。顶栏的设置/退出矮一些（40 px），不是高频操作。
- 宽度由 flex 均分得出：首页 6 列 → (1024−20−5×10)/6 = 159；设置/调试页 4 列 →
  (1024−20−3×10)/4 = 243.5；外部蜂鸣器页 3 列 → 328。整数取整后可能有 ±1 像素的差异。
- 顶栏高 66 px（屏高 11%）；普通页底栏高 202 px = 2×78 + 行距 26 + 上下边距 10×2；
  中间内容区 332 px。底栏高度按**所有页里最多的行数**算（`maxRows()`，首页 2 行），
  而**行在底栏里垂直居中**（`LV_FLEX_ALIGN_CENTER`）：首页 2 行正好排满，
  只有 1 行的设置/调试页上下各留一半（所以按钮 y 是 460 而不是 408）。
  这样普通页切页时内容区不会上下跳。
- **信息源覆盖层**按 1024×600 算：顶栏 60 px、左侧 225 px、右侧 245 px、中间 554 px；
  左侧按钮 56 px 高、宽约 201 px；覆盖层顶栏的【首页】220×42、【退出应用】160×42；
  右侧下拉框 100%×44，"占位"/"应用设置" 100%×44。
---

## 6. 可选依赖（CMake 选项）

| 选项 | 默认 | 作用 | 不满足时 |
|---|---|---|---|
| `WITH_GUI` | `AUTO` | 编 LVGL 界面（`deffire-gui-dev`） | 原生：警告并跳过；交叉：`ON` 时直接报错 |
| `WITH_GPIOD` | `AUTO` | 编 `gpio_read_value()`（GPIO 开关量输入，传感器用） | 明确打印一句"不编入产物"并继续；`ON` 时直接报错 |

```bash
cmake -B build -S . -DWITH_GPIOD=OFF     # 完全不依赖 libgpiod
cmake -B build -S . -DWITH_GPIOD=ON      # 必须要有可用的 libgpiod, 否则报错
```

`AUTO` 的意思是"有**可用版本**就编、否则跳过，**并且说一声**"——跳过时
`gpio_read_value()` 仍然存在，但会打印一行提示并返回 `-1`（不静默返回"低电平"
这种假数据）。

> ⚠ **只支持 libgpiod v1**：代码里用的是 `gpiod_chip_open_by_label` /
> `gpiod_line_request_input` 这套 v1 API，**v2 把这些符号全删了**。
>
> | 环境 | libgpiod | 结果 |
> |---|---|---|
> | armhf 容器 / 板子（Debian 12 bookworm） | 1.6.3 | ✅ 编入 |
> | Fedora 44（原生开发机） | 2.2.5 | ❌ 找到但不编入（`ON` 时报错） |
>
> 所以**原生 Fedora 构建拿不到这个功能**，装了 `libgpiod-devel` 也一样；要用就得先把
> `gpio_read_value()` 移植到 v2 API。交叉编译/板上不受影响。

板载 LED / 蜂鸣器**不需要** libgpiod（走 `/sys/class/leds/*`）；要它的是接在
GPIO 上的开关量输入传感器 —— 接线表在 `src/library/start.cpp` 的 `kGpioInputs`（现在是空的，
要上板实测后填，见 §10）。
---

## 7. VS Code 配置

`.vscode/` 下三个文件：`tasks.json`（10 个任务）、`settings.json`、`c_cpp_properties.json`。

`Ctrl+Shift+B` = 原生全量构建。GUI 相关：

| 任务 | 说明 |
|---|---|
| `CMake: 构建 GUI 并运行 (原生预览)` | 编 x86-64 版并弹出窗口 |
| `CMake: 构建 GUI armhf (交叉编译)` | 调 `build.sh gui-armhf`（容器） |

IntelliSense 走 `compile_commands.json`（CMake 导出），所以补全/跳转/报错与真实编译一致。
**前提是先跑一次配置**（`Ctrl+Shift+B` 会做）。
---

## 8. 设计要点

- **一套骨架，两个产物**：`cpp_start()`（`src/library/start.cpp`）里是**唯一的主循环**，
  四个阶段的声明在 `include/start.h`：

  ```cpp
  int  device_init();                              // 硬件/配置, 一次
  int  status();                                   // 各模块采集(内部节流 200 ms)
  int  gui_build();                                // 建界面, 一次
  int  render();                                   // 刷一帧
  void gui_teardown();                             // 收尾

  device_init();                                   // ↓ cpp_start() 里
  if (gui_build() < 0) return 1;
  while (!g_quit) { status(); render(); }
  gui_teardown();
  ```

  加模块只动 `status()`，改界面只动 `gui.cpp`，找循环只看 `cpp_start()` 一处。
  **程序级全局变量**（`g_quit` / `g_argc` / `g_argv`）定义在 `src/source/main.cpp`，
  GUI 内部句柄（`g_disp` / `g_font_*` / `g_page` …）留在 `gui.cpp`，不露到全局。
  控制台版把 `gui_build/render/gui_teardown` 换成 `src/appgui/gui_stub.cpp` 的空实现，
  所以 `main.cpp` 和 `cpp_start()` 只有一份，没有 `#ifdef`。
- **逻辑层独立成静态库**：`greeting.cpp` → `libfirecontrol.a`，主程序与单元测试都链接它，
  这样逻辑代码能被测试覆盖、也能交叉编译（不依赖平台特有库）。
- **GUI 用 LVGL + X11 后端**：而不是 framebuffer —— 板上 `/dev/fb0` 归 Xorg 管，直写会抢屏。
  将来若去掉 Xorg，把显示初始化换成 `lv_linux_fbdev` + `lv_evdev` 即可，界面代码不用动。
- **中文字体走 FreeType 运行时加载**：改文案不用重新生成字库。字体按**文件路径**查找而非
  字体名，避免 fontconfig 静默替换成不含汉字的字体。
- **默认按屏幕尺寸建窗口**：无参数时窗口按屏幕实际尺寸建；板上跑着 Openbox，
  是否铺满由 DE 决定（见 §3 与 [workflow.md](workflow.md) §2.2）。

### 8.1 JSON 状态（最基础）

`src/library/json_status.cpp` 用 **cJSON 1.7.18**（`third_party/cjson`，MIT）解析一个最小的
状态对象：

```json
{"data":1,"smoke":1}
```

- 只认根对象里的 **`data`** 和 **`smoke`**，且必须是数字；其它字段忽略。
- 解析失败不退出程序：`JsonStatus::valid=false`，`error` 里是静态原因（如“缺少 smoke”），
  控制台和界面都显示“未识别”。
- 数据来源优先级：
  1. `FIRECONTROL_JSON` 环境变量里的字面量（调试最方便）；
  2. `FIRECONTROL_JSON_FILE` 指定的文件；
  3. 当前目录的 `firecontrol.json`（存在才用；板上按 `cd ~/Desktop` 启动时就是它）；
  4. 内置示例帧 `{"start":1}{"data":1,"smoke":1}{"end":1}`。

默认文件是相对**当前工作目录**的；从别的目录启动就找不到它，用
`FIRECONTROL_JSON_FILE=/绝对路径`，或先 `cd ~/Desktop` 再启动。
- 来源是文件时，`status()` 每 200 ms 重读一次；只有值/错误变化才递增 `revision`，
  GUI 只在 `revision` 变化时重设标签，不会每帧触发重排。
- 控制台在变化时打印 `--- JSON 状态 ---` + `完整报文: ...` + `data=...  smoke=...`。
- 界面：内容区页名下面一行 `JSON  data = 1    smoke = 1`；失败显示 `JSON 未识别 (...)`。
  信息源页底部还有一行 `完整报文：...`，只在收到完整帧后更新。

示例：

```bash
# 字面量 (完整帧)
FIRECONTROL_JSON='{"start":1}{"data":2,"smoke":0}{"end":1}' ./build/deffire-gui-dev

# 文件（运行时追加/重写，界面 200 ms 内跟着变）
echo '{"start":1}{"data":1,"smoke":1}{"end":1}' > firecontrol.json
./build/deffire-gui-dev
```

当前它**只是解析/显示**，还没有接串口、网络或真实传感器；也没有根据 `smoke` 做报警判定。

cJSON 是**静态编入**的：`build-armhf/deffire-gui-dev` 的 `NEEDED` 里不会出现
`libcjson.so`，板上不需要装 cJSON；按原来的“本机交叉编译 → 拷到板上”流程即可。

### 8.2 JSON 帧与鲁棒性

传感器端定义的帧：

```text
{"start":1} ...数据... {"end":1}
```

实现拆成两块：

- `src/library/json_frame.cpp`：纯流式帧解析，不依赖 cJSON，可单独测试；
- `src/library/json_status.cpp`：把完整帧里的 payload 交给 cJSON 解析。

`JsonFrameParser` 的行为：

- 只按**精确字节**匹配 `{"start":1}` / `{"end":1}`；
- `feed()` 可以一次给一段，标记被拆到两个 chunk 也能接上；
- 没看到 `start` 之前的数据全部丢弃；
- 看到 `start` 后如果又出现新的 `start`（旧帧没有 `end`），**丢弃旧帧，从新 start 重新收集**；
- 收到完整 `start...end` 才 `pop()` 出一个帧；
- 单帧超过 8 KB 就丢掉当前帧，等下一个 start，防止缓冲区无限增长；
- 一次 feed 里可以切出多个完整帧。
- 标记按精确字节匹配，payload 的字符串里**不要**出现 `{"start":1}` / `{"end":1}`
  这组字面量，否则会被误切。

**谁产生报文头/尾（约定，2026-10-31 明确）**：

- 头/尾归**发送方（传感器模块）**：app 只解析、不添加。`JsonFrameParser` 收到 start 之前
  的字节直接丢弃，解析器本身从不写标记；`extractPayload()` 只是把标记切掉。
- app 里唯一自己产出的成品帧是 `json_status.cpp` 的 `kSampleFrame`（没配置来源时的占位
  示例，比如 `FIRECONTROL_JSON` 不设、`./firecontrol.json` 不存在时），它**不参与真实
  数据**；除它之外没有任何地方拼帧、补头补尾。
- 因此"模块已经自带报文头/尾"**不需要**我们在 app 里再做一步：要做的只有让
  `json_frame.h` 的 `kFrameStart` / `kFrameEnd` 等于模块**实际发送**的字节。
- ⚠ 这两个值现在**写死**为 `{"start":1}` / `{"end":1}`，还没与模块实测比对。对不上时不会
  报错 —— 整路永远"暂无"（字节全在 start 之前被丢掉），所以接模块前必须先确认字节。
- **有意的不对称**：文件来源接受整段 JSON（`feedWholeText()` 的 legacy 分支 —— 文件内容
  天然就是一条完整消息，有边界，不需要分隔符）；字节流（`json_source_feed()`）则必须带
  start/end —— 流没有消息边界，不靠分隔符切不出消息。等模块协议确定后二选一：用它的
  头/尾切，或者由传输层保证"一次读 = 一条完整报文"，**不要两头都猜**。
- 另一个待确认的接口层问题：接口页右栏选的是 **GPIO 端口 + 线号**（对应
  `gpio_read_value()`，开关量 0/1），而 `json_source_feed()` 收的是 **JSON 字节流**；
  两者之间目前没有桥，且 `json_source_feed()` **没有调用点**（【应用设置】只保存选择）。
  接硬件前要定：模块是把整条报文当字节流送进来（串口/其它总线），还是只有开关量电平
  —— 后者的话根本不会有 JSON，五行数据要按电平算。
- **发送方向**：app 现在**没有**任何向传感器发报文的代码（`gpio_write_value()` /
  `gpio_output_*()` 只用于外部无源蜂鸣器 PA6）。接收不需要我们发东西；只有模块是
  "查询-应答"式时，才需要我们自己构造并发出请求报文 —— 这一样要等模块手册/协议。

文件来源的处理：

- 文件内容不变 → 不重复喂，避免同一帧每 200 ms 重复发布；
- 文件被**追加**（新内容以旧内容为前缀）→ 只把新增后缀喂给解析器，跨刷新的半个帧能接上；
- 文件被**重写** → 重置解析器，从头收集；
- 之前在帧模式、但重写后的内容没有 `start` → 按“不完整”处理，**保留上一帧完整报文**，不把半截内容当新帧；
- 读文件失败 → 保留上一帧，只更新错误状态。

界面：

- 每个信息源有**独立**的 `JsonFrameParser` / `JsonStatus`；接口页「收到的报文」
  只显示当前信息源的完整帧。收到帧时该路 `revision` 强制递增，卡片刷新
  （长报文自动换行，卡片可上下滚动）。
- 还没有接串口时，测试来源是：
  - 信息源 1：`FIRECONTROL_JSON` / `FIRECONTROL_JSON_FILE` / `./firecontrol.json` / 内置帧；
  - 信息源 N（2..12）：`FIRECONTROL_JSON_FILE_<N>`（可选）。
- 串口接上后，每路读取线程收到字节调用 `json_source_feed(index, data, len)` 即可；
  `index` 是 0..11。这个接口是线程安全的。现在**没有硬编码任何串口设备路径/波特率**，
  等接线确认后再加。

各项的设计取舍与踩过的坑在 [WARNING.md](WARNING.md)。
---

## 9. 实测结果

| 项目 | 结果 |
|---|---|
| 原生构建 | 0 warning，`ctest` 3/3 |
| JSON 解析 | cJSON 1.7.18（vendored）；`test_json_status` 覆盖合法/缺字段/类型错误/语法错误/尾部垃圾 + 逐路 feed/reset；`test_json_frame` 覆盖分块到达、半包、不完整帧丢弃、新 start 重开、多帧；控制台/本地 UI 已验证逐路文件（信息源 2 显示自己的帧，其他路不受影响）；**板上串口待接** |
| 原生 GUI | `deffire-gui-dev` 约 900 KB，1024×600 窗口正常；首页 12 个信息源、设置页、信息源页、LED / 蜂鸣器 / 外部蜂鸣器页的切页与按钮派发已实测（含文件 JSON 热更新） |
| 容器镜像构建 | ✅ 通过（`bookworm-slim` + gcc-arm 12 + cmake 3.25.1） |
| 镜像自检 | ✅ 能交叉编译 X11+FreeType 的 armhf 程序；pkg-config 指向 armhf |
| 容器内交叉编译 | ✅ LVGL 全量 + `deffire-gui-dev` 链接成功 |
| **armhf 产物** | **约 0.4 MB**（具体字节以构建产物为准），`ELF32 / ARM / hard-float`，依赖 `libX11` / `libfreetype` / **`libgpiod`** / `libstdc++` / `libgcc_s` / `libc`；同源同镜像重建 sha256 逐字节一致（可复现） |
| 上板实测（LVGL 版） | ✅ **已上板验证通过**（2026-10-18，项目成员确认；逐项证据建议按 [VERIFY-RUNBOOK.md](VERIFY-RUNBOOK.md) §5 回填） |
| 上板实测（老 Xlib+Xft 版） | ✅ 曾通过（窗口正常、中文正常），但该版本已被 LVGL 版取代 |

---

## 10. 待办

- [x] ~~**在板子上实测 LVGL 版**~~：2026-10-18 已上板验证通过（具体覆盖项以板上记录为准）。
- [x] ~~板上实测触摸~~：随上板验证确认；若后续发现坐标偏移，按 VERIFY-RUNBOOK §4.2/§4.3 记录。
- [x] ~~确认铺满时字号观感~~：上板验证已覆盖；要微调只改 `kBigFontPx` / `kSmallFontPx`。
- [x] ~~给字体候选表补上 Fedora 的路径~~ —— 改成「候选表没命中就扫目录」（见 WARNING.md C-3）
- [ ] **全屏交给 DE**：不再自己实现。按 [workflow.md](workflow.md) §2.2 向 LXDE/Openbox
      发 `_NET_WM_STATE_FULLSCREEN` 请求（或配窗口规则），确认一次铺满即可；
      不在 GUI 里手工算坐标 / 造无边框窗口（见 WARNING.md C-7）
- [ ] **填 12 组默认 GPIO 映射**：`kDefaultSensorConfig[12]` 现在全是 `-1`（未配置）；
      项目成员提供实际端口/线号后填进去，启动时就会显示"已选择 ..."。
      没填的模块仍可在 app 内手动选
- [ ] **信息源页的数据行接真实数据**：页面已有 `烟雾 / 火焰 / 温度 / 蜂鸣器状态 / LED 状态`
      五行；现在烟雾/火焰是默认 `0（正常）`，温度/蜂鸣器/LED 是占位符。接上数据源后
      更新 `g_sensor_values[5]`，不要预置假读数
- [ ] **确定蜂鸣器/LED 联动规则**：目前两行都显示 `--（规则待定）`，还没根据烟雾/火焰/
      温度计算；规则定下来后再写判定并驱动对应输出
- [ ] **确定温度阈值/单位**：温度目前显示 `--（数值待接入）`，阈值和单位都未定
- [ ] **把 12 个信息源入口接到真实数据源**：当前 JSON 只显示全局 `data` / `smoke`，
      还没有按信息源拆分；需要先有模块 ID → 通道的映射（见 workflow.md §2.5）
- [ ] **外部无源蜂鸣器的音调还没调**：板上已确认"能响"，但没扫过频率。用
      `FIRECONTROL_BUZZER_HZ=1000/2000/3000/4000` 各试一遍，找出最响的那个，
      再把 `start.cpp` 的 `kOutBuzzerDefaultHz` 改成它（当前默认 2 kHz）
- [ ] 传感器模块（当前 0%）：骨架已有（`gpio_read_value()` + `WITH_GPIOD`），但**要先上板
      确认传感器接在哪个 gpiochip / 哪条线**、那些线是否已被内核占用 —— 见
      [workflow.md](workflow.md) §2.3
- [ ] **传感器做成可扩展注册表**：探测器数量/型号未定，后续改成配置文件/运行时注册，
      支持任意 N 路；当前 `kGpioInputs` 的静态空表只是占位（见 workflow.md §2.3）
- [ ] **接 12 路独立串口**：现在每路 parser/status 和 `json_source_feed(index, data, len)`
      已就绪，但串口还没接线。需要设备路径/波特率，开串口读取线程，收到字节直接喂对应 index；
      断线重连时调 `json_source_reset(index)`。目前**没有硬编码任何设备路径**
- [ ] **板上验证逐路帧**：用 `FIRECONTROL_JSON_FILE_<N>` 分别喂 1..12，确认各页只显示
      自己的完整帧；再接真实串口验证半包/坏帧/重连
- [ ] **串口错误/超时状态**：断线、长时间无帧、CRC/格式错误怎么显示到模块状态，还没定
