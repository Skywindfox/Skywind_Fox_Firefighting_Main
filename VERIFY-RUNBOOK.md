# 上板验证 Runbook（首次与回归）

> 面向 [NOTES.md](NOTES.md) §10 待办里**必须真机做**的那几条：上板实测 LVGL 版（触摸、
> 字号观感、是否铺满），以及 Windows 上运行 `build-armhf.ps1`。
> 本文只回答四件事：**怎么执行 / 看什么算通过 / 失败怎么查 / 留什么证据**。
> 原理与坑见 [WARNING.md](WARNING.md)（LED/蜂鸣器的 trigger 语义见 C-8，sysfs 权限见 D-8，
> `build-armhf.ps1` 的 BOM 见 B-18）；板上环境见 [NOTES.md](NOTES.md) §3，构建/部署见 §2/§4。
>
> **2026-10-18 更新：首次上板验证已通过**（LVGL 版 armhf 产物），Windows 侧
> `build-armhf.ps1` 也已跑通。本文保留为**回归/复验流程**：判据仍然有效，以后每次改硬件
> 或界面都按这里逐项跑，并按 §5 回填证据。

命令前缀含义：

| 标记 | 在哪台机器执行 | 谁 |
|---|---|---|
| `[Win]` | Windows 开发机（装了 Docker Desktop） | FBI-0537 |
| `[板]` | 板子上（`ssh fbi@<板子IP>` 或板子自带终端） | 任何人 |
| `[Linux]` | Fedora 开发机 / WSL / Git Bash | Skywindfox |

约定：板上一切产物都放 **`~/Desktop`**，普通用户是 **`fbi`**。

---

## 1. `[Win]` 交叉编译：`build-armhf.ps1`（首次 / 回归）

### 1.1 前置

1. 装好并**启动** Docker Desktop（不是装完就行，要等它状态稳定、鲸鱼图标不动）。
2. 仓库更新到最新 `main`（写本文时为 `b2864fd`）：

   ```powershell
   git pull
   git log --oneline -1
   ```

3. 先记下 PowerShell 版本 —— 这个脚本的 BOM / 编码坑只在 5.1 上暴露：

   ```powershell
   $PSVersionTable.PSVersion
   ```

4. **改过这个脚本的话**，先确认 UTF-8 BOM 还在（**已被吃掉两次**，见 WARNING.md B-18）：

   ```powershell
   Format-Hex armhf-toolchain\build-armhf.ps1 | Select-Object -First 1
   # 前 3 字节必须是 EF BB BF ；在 Git Bash 里等价命令:
   #   head -c3 armhf-toolchain/build-armhf.ps1 | od -An -tx1   →   ef bb bf
   ```

### 1.2 命令

```powershell
# 记录全过程（推荐）：
Start-Transcript -Path .\build-win.log -Append

.\armhf-toolchain\build-armhf.ps1
$code = $LASTEXITCODE
"PS1 exit = $code"

Stop-Transcript
```

镜像不存在时会自动构建；想强制重建环境镜像加 `-Rebuild`：

```powershell
.\armhf-toolchain\build-armhf.ps1 -Rebuild
```

**`docker.io` 拉不动时**（`-Rebuild` 会真的去 pull 基础镜像）：

```powershell
.\armhf-toolchain\build-armhf.ps1 -Rebuild -BaseImage docker.m.daocloud.io/library/debian:bookworm-slim
```

> 坑：`.ps1` **没有** `.sh` 里那段"基础镜像可达性预检"。网络不通时它会**静默卡住**
> 等 TCP 超时。若两分钟没输出，Ctrl+C，改用上面的 `-BaseImage` 走镜像站。

### 1.3 通过判据（逐条对照，缺一不可）

| # | 该看到什么 | 说明 |
|---|---|---|
| 1 | `=== 重建镜像 ... ===` 或 `=== 镜像 ... 不存在, 开始构建 ===`，或两行都没有 | 两行都没有 = 镜像已存在、直接复用，正常 |
| 2 | `=== 容器内交叉编译 (docker) ===` | |
| 3 | 逐条 `✓ deffire-gui-dev` / `✓ deffire-dev`（各一行） | 出现 `✗ ... (docker cp 失败, 退出码 N)` = **真失败**，不要继续往下走 |
| 4 | `=== 写工具链指纹 ===` 里 `host_runtime = docker-desktop (windows)`，且 `built_local` 是**当前 UTC 时间** | `built_local` 是旧时间 = 跑的是旧产物 |
| 5 | `=== 产物 ===` 两行都有字节数（无 `缺失`） | 只有控制台产物、GUI 缺失时，脚本会 `exit 1`（这就是 PR #6 修的那个静默问题） |
| 6 | `✓ 完成: ...\build-armhf`，**且 `$LASTEXITCODE -eq 0`** | 退出码是唯一权威判据 |
| 7 | `build-armhf\TOOLCHAIN.txt` 与 `deffire-gui-dev` / `deffire-dev` 三个文件都在 | |

参考量级（Linux 侧同一套容器实测，2026-09-18）：

```
deffire-gui-dev   约 408 KB   (407936 字节)
deffire-dev       约 9.8 KB   (9768 字节)
```

两个都比接入 libgpiod 之前大（约 404 KB / 5.6 KB）：`libgpiod` 是通过 `firecontrol`
静态库链进去的，而 `gpio_read_value()` 与 `write_File()` 在同一个 `.cpp` 里，所以
**控制台程序也连带依赖 `libgpiod.so.2`**（见 workflow.md §2.3）。

字节数**只作量级参考**，每次改代码都会小幅变化；判断成败看的是"文件在不在 + 退出码"，
**不要**用字节数相等当通过条件。

### 1.4 失败分支速查

| 输出 | 含义 | 处理 |
|---|---|---|
| `找不到 docker。请安装并启动 Docker Desktop` | 没装 / 不在 PATH | 装 Desktop，**重开** PowerShell 再试 |
| `Docker 没有在运行。启动 Docker Desktop 后重试。` | Desktop 还在 starting | 等状态稳定再跑 |
| 长时间无输出 | 在 pull 基础镜像（无预检） | Ctrl+C → 加 `-BaseImage <镜像站>` |
| `✗ 无法从 Dockerfile 解析基础镜像名` | `Dockerfile` 里 `ARG BASE=` 那行被改坏 | 用 `-BaseImage ...` 显式给 |
| `镜像构建失败` | 多为基础镜像拉不到 | 换镜像站重试 |
| `容器内构建失败` | 容器里 cmake/g++ 报错 | 往上翻 grep 出的 `error` 行；必要时 `-Shell` 进去手工复现 |
| `旧产物删不掉: ...` | 文件被杀毒 / 编辑器 / OneDrive 占用 | 关掉占用者；或删整个 `build-armhf\` 再跑（脚本故意在这里硬失败） |
| `缺产物: deffire-gui-dev` | 容器里 GUI 没编出来 | 当失败处理，别拿旧产物上板 |

### 1.5 必须留的证据（回填 §5 用）

```powershell
# 1) 两个产物的哈希（与 Linux 侧 c73d670c… / 6ddb8427… 相同是加分项：
#    说明两边可复现；不同**不算失败**，但必须过 §3 的板上预检才算数）
Get-FileHash -Algorithm SHA256 build-armhf\deffire-gui-dev
Get-FileHash -Algorithm SHA256 build-armhf\deffire-dev

# 2) 产物大小
Get-ChildItem build-armhf\deffire-gui-dev, build-armhf\deffire-dev | Select-Object Name, Length

# 3) 工具链指纹（整份贴回去）
Get-Content build-armhf\TOOLCHAIN.txt
```

加上 §1.2 的 `build-win.log` 全文与 `$LASTEXITCODE`。

---

## 2. 传板

### 2.1 `[Win]` scp 三件套

```powershell
$B = "fbi@<板子IP>"
scp build-armhf\deffire-gui-dev build-armhf\TOOLCHAIN.txt armhf-toolchain\setup-board-permissions.sh "${B}:~/Desktop/"
```

（`deffire-dev` 是控制台自检程序，不跑 GUI 时可不传。`~/Desktop/` 的 `~` 由**远端 shell** 展开，Windows 侧不用管。）

`[Linux]` 等价一步到位（会自动建目录 + 传完直接探测 sysfs 写权限）：

```bash
./armhf-toolchain/deploy-to-board.sh fbi@<板子IP>      # 加 --run 传完直接启动
```

### 2.2 `[板]` 先装运行时库（否则界面根本起不来）

```bash
sudo apt install libgpiod2
```

产物链着 `libgpiod.so.2`（`gpio_read_value()` 用它）。**缺这个库不是"传感器不能用"，
是整个 GUI 起不来** —— 动态链接器在 `main()` 之前就报
`error while loading shared libraries: libgpiod.so.2`。§3 的预检会把缺的库列出来。

不想要这个依赖就在构建时 `-DWITH_GPIOD=OFF` 重新交叉编译（NOTES.md §6）。

（顺带：要看 GPIO 接线/占用情况时装上工具 —— `sudo apt install gpiod`，
里面是 `gpiodetect` / `gpioinfo`。传感器接线确认见 workflow.md §2.3。
注意这两个命令**以 fbi 身份跑会 `Permission denied`**，要先做 §2.3 的权限配置。）

### 2.3 `[板]` 首次：一次性权限配置

```bash
sudo bash ~/Desktop/setup-board-permissions.sh fbi
```

然后**必须让 leds / gpio 组生效**，三选一：

| 做法 | 何时够用 |
|---|---|
| 拔电重启 / `sudo reboot` | 最稳，一次解决 |
| 注销后重新登录桌面 | 等价，比重启轻 |
| `newgrp leds` | **只对当前那个 shell 有效**（`gpio` 组同理）。从桌面会话启动 GUI 时不够 —— 桌面会话的组还是旧的 |

> 注意：脚本自己最后打印的是"重新登录或 `newgrp`"，而 `deploy-to-board.sh` 的提示是 `sudo reboot`。
> 两者都对，差别只在上面的适用范围。**从 ssh 会话启动 GUI 时，那次 ssh 就是新登录，组已经生效，
> 不重启也能用**；从板子桌面自己的终端启动才必须重新登录/重启。

`[板]` 不用 sudo 验证（必须能写，否则界面按钮点了没反应）：

```bash
echo 1 > /sys/class/leds/beep/brightness   # 应该响
echo 0 > /sys/class/leds/beep/brightness   # 停
ls -l /sys/class/leds/sys-led/brightness /sys/class/leds/sys-led/trigger \
      /sys/class/leds/beep/brightness     /sys/class/leds/beep/trigger
id -nG                                     # 应含 leds gpio

# gpio 是**另一个独立的坑**（子系统不同，需要第二条 udev 规则，见 WARNING.md D-9）
ls -l /dev/gpiochip*                       # 组应是 gpio
gpiodetect                                 # 应能列出 gpiochipN，不再是 Permission denied
```

---

## 3. `[板]` 预检：产物与本板是否匹配

`[Linux]` 从开发机远程查（会把产物传上去再查）：

```bash
./armhf-toolchain/verify-on-board.sh fbi@<板子IP>
```

或 `[板]` 就地查（不依赖开发机）：

```bash
cd ~/Desktop
bash verify-on-board.sh ./deffire-gui-dev ./TOOLCHAIN.txt   # 脚本需先 scp 上去
```

**通过判据**：末尾打印 `==== 结论: 产物与本板匹配, 可以运行 ====`，且退出码 0。

必须同时确认这几项**不是**被跳过的：

- 出现 `✗ 板上缺 readelf/file/ldconfig` 时，脚本**故意判失败**（WARNING D-2 的教训）。
  装齐再跑：`sudo apt install binutils file`。**不要**接受一次"跳过了检查却报匹配"的结果。
- 三个字体路径之一被 `✓` 命中（板子上应是 `/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc`）。
- `NEEDED` 逐个 `✓`（预期 `libX11.so.6`、`libfreetype.so.6`、`libgpiod.so.2`、`libstdc++.so.6`、`libgcc_s.so.1`、`libc.so.6`；少任何一个都要先解决，`libgpiod.so.2` 缺了整个界面起不来）。
- 记录：`uname -m` / `uname -r` / glibc 版本 / 内存 / `/tmp/.X11-unix/X0` 是否存在。

---

## 4. `[板]` 运行 + 逐项验收（首次 / 回归的主体）

### 4.0 启动并留日志

```bash
cd ~/Desktop
DISPLAY=:0 ./deffire-gui-dev 2>&1 | tee run-$(date +%Y%m%d-%H%M%S).log
```

（回调里每处都 `fflush(stdout)`，所以管道给 `tee` 不会丢输出。）

### 4.1 启动横幅该长什么样

```
已连接 X server
  服务端 : <vendor>
  屏幕   : 1024x600, 深度 <N>
  窗口   : 1024x600 (按屏幕尺寸)
  字体(主标题  ): /usr/share/fonts/.../wqy-zenhei.ttc  [40 px]
  字体(按钮标签): /usr/share/fonts/.../wqy-zenhei.ttc  [18 px]
  字体(状态文字): /usr/share/fonts/.../wqy-zenhei.ttc  [16 px]
  布局   : 深色工业风 —— 顶栏(品牌/状态灯/设置/退出) + 内容区 + 分页底栏 (6 页, 首页 12 个信息源)

交互:
  点右上角【退出】   退出程序
  点右上角【设置】   进设置页 (LED / 蜂鸣器 / 外部无源蜂鸣器调试入口)
  首页点【信息源 1..12】 进模块页, 页内【返回】回首页
  设置页点【LED 调试】/【蜂鸣器 调试】/【外部无源蜂鸣器】 进对应调试页
  调试页里点按钮     单独控制板载 LED / 蜂鸣器 / 外部无源蜂鸣器
  按 q / Esc        退出程序 (每一页都一样)
```

任何一行缺失或报 `无法连接 X server` / `创建 X11 窗口失败` / `警告: 加载字体失败`，先解决再往下走。

### 4.2 按钮逐项验收表

界面是**分页**的（顶栏所有页一致，切页只换底栏按钮）：

```
首页 (2 行 × 6 列) : 信息源 1 ｜ 信息源 2 ｜ … ｜ 信息源 12
信息源 N 页       : 全屏覆盖层：顶栏(火灾控制终端/首页/退出应用) + 左侧信息源列表
                    + 中间接收/模块状态卡片 + 右侧信息源设置
设置页             : LED 调试 ｜ 蜂鸣器 调试 ｜ 外部无源蜂鸣器 ｜ 返回
LED 调试页         : LED 开 ｜ LED 关 ｜ LED 心跳 ｜ 返回
蜂鸣器调试页       : 蜂鸣器开 ｜ 蜂鸣器关 ｜ 蜂鸣器心跳 ｜ 返回
外部无源蜂鸣器页   : 外部蜂鸣器 开 ｜ 外部蜂鸣器 关 ｜ 返回    ← 没有"心跳"(它不归内核管)
```

底栏高度所有页一致（按最多的首页 2 行算），且**行在底栏里垂直居中** —— 所以
**设置/信息源/调试页只有 1 行时会上下各留一段空位**（按钮 y≈460，不是 408）。这是有意的
（换来切页时内容区不上下跳），不是渲染问题。

所以每个硬件按钮要**先点右上角【设置】**进设置页，再点对应调试入口；页内末位是【返回】。
首页的 12 个信息源点进去是全屏接口页：左侧 12 路、中间接收/报文卡片、右侧串口设置。
还没选串口时模块状态显示 `未选择串口（选好设备/波特率后点【应用设置】）`；
接收卡五行在没收到帧时显示 `--（待接入）`，这是空状态，不是渲染失败。
内容区那行小字会显示 `页名 · 上一步结果`（例如 `LED 调试页 · LED 已点亮`）——
不连控制台也能看到刚才那一下成没成，验收时顺手核一下它和控制台是否一致。

**判据同时看三处**：控制台打印 / 硬件真实反应 / sysfs 旁证。
**只看到"已点亮"不算通过** —— 那是 `write_File` 的返回值，不是硬件状态
（**不要凭推断断定硬件行为**，翻过两次车）。

| 页面 | 点击 | 控制台应打印 | 硬件应表现 | sysfs 旁证 |
|---|---|---|---|---|
| （首页） | 设置 | `点击【设置】坐标 (x,y) → 进入设置`，随后 `切换到 设置 (底栏 4 个按钮)` | 底栏换成 4 个本地测试入口 | — |
| （首页） | 信息源 1 | `点击【信息源 1】坐标 (x,y) → 进入信息源 1`，随后 `切换到 信息源 1 (底栏 1 个按钮)` | 出现全屏接口页：顶栏(首页/退出应用)、左侧信息源列表、中间接收卡片 + 模块状态/报文卡片、右侧信息源设置 | — |
| 信息源页 | 选串口设备/波特率后点【应用设置】 | `信息源 1 应用串口: /dev/ttyS0 @ 115200 (已打开)` | 模块状态变成 `<设备> @ <波特率>（已打开，等待数据）` | — |
| 信息源页 | 顶栏【首页】 | `… → 返回首页`，随后 `切换到 首页 (底栏 12 个按钮)` | 覆盖层关闭，回到普通首页 | — |
| 设置页 | LED 调试 | `点击【LED 调试】坐标 (x,y) → 进入 LED 调试页`，随后 `切换到 LED 调试页 (底栏 4 个按钮)` | 底栏换成 4 个按钮 | — |
| LED 调试页 | LED 开 | `点击【LED 开】坐标 (x,y) → LED 已点亮` | LED 常亮 | `sys-led/brightness` = `1`，`trigger` = `none` |
| LED 调试页 | LED 关 | `… → LED 已熄灭` | LED 灭 | `brightness` = `0` |
| LED 调试页 | LED 心跳 | `… → LED 心跳已开启` | LED 周期性闪 | `sys-led/trigger` = `heartbeat` |
| LED 调试页 | 返回 | `… → 返回首页`，随后 `切换到 首页 (底栏 12 个按钮)` | 底栏换成首页 12 个信息源 | — |
| 设置页 | 蜂鸣器 调试 | `… → 进入蜂鸣器调试页`，随后 `切换到 蜂鸣器调试页 (底栏 4 个按钮)` | 底栏换成 4 个按钮 | — |
| 蜂鸣器调试页 | 蜂鸣器开 | `… → 蜂鸣器已响` | 持续响 | `beep/brightness` = `1`，`trigger` = `none` |
| 蜂鸣器调试页 | 蜂鸣器关 | `… → 蜂鸣器已停` | 停 | `brightness` = `0` |
| 蜂鸣器调试页 | 蜂鸣器心跳 | `… → 蜂鸣器心跳已开启` | 间歇响 | `beep/trigger` = `heartbeat` |
| 蜂鸣器调试页 | 返回 | `… → 返回首页` | 底栏换成首页 12 个信息源 | — |
| 设置页 | 外部无源蜂鸣器 | `… → 进入外部无源蜂鸣器调试页`，随后 `切换到 外部无源蜂鸣器调试页 (底栏 3 个按钮)` | 底栏换成 3 个按钮 | — |
| 外部无源蜂鸣器调试页 | 外部蜂鸣器 开 | `… → 外部无源蜂鸣器已开`，且**启动时打过一行** `外部无源蜂鸣器: 2000 Hz 方波 (半周期 250 us)` | **PA6 上的蜂鸣器响**（2 kHz 方波；可用 `FIRECONTROL_BUZZER_HZ` 换频率） | 见下面的 `gpioinfo` 旁证 |
| 外部无源蜂鸣器调试页 | 外部蜂鸣器 关 | `… → 外部无源蜂鸣器已停` | 停 | `gpioinfo` 里该线仍是 `out` |
| 外部无源蜂鸣器调试页 | 返回 | `… → 返回首页` | 底栏换成首页 12 个信息源 | — |
| 设置页 | 返回 | `… → 返回首页` | 底栏换成首页 12 个信息源 | — |
| （任意页） | 退出（右上） | `点击【退出】按钮, 退出` → 再打印 `退出` | 窗口消失、进程结束 | 退出码 0 |

旁证命令（另开一个 ssh 会话，或点一下、切回终端敲一下）：

```bash
cat /sys/class/leds/sys-led/brightness;  grep -o '\[[a-z-]*\]' /sys/class/leds/sys-led/trigger
cat /sys/class/leds/beep/brightness;     grep -o '\[[a-z-]*\]' /sys/class/leds/beep/trigger

# 外部无源蜂鸣器走的是 libgpiod, 不在 sysfs 里 —— 看 gpioinfo:
# 按下【外部蜂鸣器 开】之后, GPIOA 的 line 6 应变成 output 且 consumer = "out_buzzer"
gpioinfo | grep -E 'GPIOA|out_buzzer' -A0 | head -20
```

**额外必测一组（回归 `0b8d30d` 那个修复）**：在 LED 调试页先点【LED 心跳】，**再**点【LED 开】。
正确结果：LED 变**常亮**（程序先写 `trigger=none` 再写亮度）。
若它还在闪 → trigger 覆盖问题回来了，要在 issue 里写明。

### 4.3 界面 / 触摸 / 铺满（主观项，仍需逐条给结论）

| 项 | 怎么看 | 通过标准 |
|---|---|---|
| 深色底对比度 | 正常室内光下看 | 三段（顶栏/内容区/底栏）分得清；次要小字（状态说明）也读得出来 |
| 按下态 | 手指按住按钮不放 | 有**立刻**的视觉反馈（按下态底色变化），松手恢复 |
| 触摸命中 | 点每个按钮，看控制台打印的 `坐标 (x,y)` | 坐标落在该按钮实际区域内、靠近中心；**重点测四角与右上【退出】** |
| 坐标偏移 | 先用手指点屏幕四角附近再点中央，记录 5 组坐标 | 若出现**系统性**偏移（固定差值 / x-y 互换 / 缩放），记下趋势 |
| 中文显示 | 看按钮标签与顶栏 | 汉字正常，无方框/缺字 |
| **铺满屏幕** | 看窗口有没有 Openbox 标题栏、边框、四周留边 | 内容铺满 1024x600，无标题栏遮挡 |

全屏的旁证（板上**装了才有**，没装就先别装，拍照足够）：

```bash
DISPLAY=:0 xdotool getactivewindow getwindowgeometry
DISPLAY=:0 wmctrl -lG
```

预期（2026-11 起）：程序启动后会向 WM 请求 `_NET_WM_STATE_FULLSCREEN`（控制台打印
`全屏   : 请求 _NET_WM_STATE_FULLSCREEN…`），窗口应无标题栏、铺满 1024×600，
底栏按钮完整可见。若仍被 lxpanel/标题栏挡住：

1. 先确认请求是否生效（装了才有）：
   ```bash
   DISPLAY=:0 xprop -id "$(xdotool search --name FireControlApp | head -1)" _NET_WM_STATE
   ```
2. 不生效就按 [WARNING.md](WARNING.md) C-7 的 DE 侧兜底：lxpanel 自动隐藏，或
   `~/.config/openbox/rc.xml` 里给 `FireControlApp` 配 `<decor>no</decor>` +
   `<fullscreen>yes</fullscreen>`，然后 `openbox --reconfigure`。
3. **不要**改界面代码去手工算坐标/减法 —— 布局是 1024×600 设计, 差几像素就重画。

### 4.4 JSON 帧 / 状态

帧格式和 payload（字段名、语义都来自模块固件）：

```text
{"start":1} ...数据... {"end":1}
{"temperature":36.5,"fire_state":1,"smoke_state":0,"buzzer_state":1,"led_state":3}
```

- `temperature` 浮点；`fire_state` / `smoke_state` 是 0/1（0 无、1 有）；
- `buzzer_state`：0 灭 / 1 急促(100ms) / 2 缓慢(500ms) / 3 长响；
- `led_state`：0 灭 / 1 蓝闪(烟) / 2 绿闪(温) / 3 红闪(火) / 4 蓝绿 / 5 蓝红 / 6 绿红 / 7 轮转；
- 字段**都可选**；旧名 `fire_alarm` / `smoker_alarm` / `buzzer_alarm` 作别名仍认，
  旧 `data`/`smoke` 也兼容。

默认内置示例帧是
`{"start":1}{"temperature":25.5,"fire_state":0,"smoke_state":0,"buzzer_state":0,"led_state":0}{"end":1}`。
普通页内容区页名下面应显示 `JSON  温度=25.5  火焰=0  烟雾=0  蜂鸣=0  LED=0`；
信息源接口页「收到的报文」应显示上面那条完整帧；接收卡五行应显示
烟雾 `0 无烟` / 火焰 `0 无火` / 温度 `25.5 阈值待定` / 蜂鸣器 `0 灭` / LED `0 灭`。
控制台在第一轮 `status()` 会打印：

```text
--- JSON 状态 ---
  完整报文: {"start":1}{"temperature":25.5,...}{"end":1}
  temperature=25.5  fire_state=0  smoke_state=0  buzzer_state=0  led_state=0
```

| 场景 | 怎么触发 | 通过标准 |
|---|---|---|
| 默认示例 | 直接在 `~/Desktop` 启动 | 普通页列出五个字段；接口页“收到的报文”带 start/end；五行按上面对应 |
| 完整帧字面量 | `FIRECONTROL_JSON='{"start":1}{"temperature":40,"fire_state":1,"buzzer_state":1,"led_state":3}{"end":1}' DISPLAY=:0 ./deffire-gui-dev` | 普通页 温度=40.0 / 火焰=1；火焰行 `1 有火`、蜂鸣器 `1 急促`、LED `3 红闪` |
| 旧字段名 | `FIRECONTROL_JSON='{"start":1}{"fire_alarm":1,"smoker_alarm":1,"buzzer_alarm":2}{"end":1}'` | 别名被认：火焰 `1 有火`、烟雾 `1 有烟`、蜂鸣器 `2 缓慢` |
| 不完整帧 | 先写 `{"start":1}{"temperature":3,` 启动 | **不发布**，接口页「收到的报文」显示"暂无"（或保留上一帧） |
| 补齐后续 | 在上面文件里追加 `"fire_state":0}{"end":1}` | 200 ms 内出现完整帧，控制台打印一次 |
| 坏帧后接新帧 | 写入 `{"start":1}{"temperature":9,{"start":1}{"temperature":2,"fire_state":0}{"end":1}` | 旧的不完整帧被忽略，只发布后一个完整帧 |
| 非法 payload | `{"start":1}{"foo":1}{"end":1}`（只有未知字段） | 接口页报文显示，但报"没有已知字段"，程序不退出 |
| 字段类型错 | `{"start":1}{"temperature":"hot"}{"end":1}` | 报"temperature 不是数字"，程序不退出 |
| 逐路文件 | `FIRECONTROL_JSON_FILE_2=<完整帧文件>` 启动，进信息源 2 | 信息源 2 显示该帧；信息源 1/3 不显示它 |

> 2026-11 原生实测（伪终端串口，`/dev/pts/N` @115200）：两帧交替喂入，五行分别显示
> `0 无烟 / 1 有火 / 36.5 阈值待定 / 1 急促 / 3 红闪` 和
> `1 有烟 / 0 无火 / 37.2 阈值待定 / 2 缓慢 / 5 蓝红`，与报文一致。

### 4.5 串口来源

界面：信息源页右栏 **串口设备** / **波特率** / 【刷新串口】/【应用设置】。
两个下拉都选了才会打开；有一个没选 = 取消配置并关端口。模块状态应显示
`已打开，等待数据`；打开失败显示 `未打开：<原因>（每秒重连）`。

| 场景 | 怎么触发 | 通过标准 |
|---|---|---|
| 列表枚举 | 点【刷新串口】 | 控制台打印 `串口列表已刷新 (N 个设备)`；下拉里出现 `/dev/tty*` |
| 打开串口 | 选设备+波特率→【应用设置】 | 控制台 `信息源 N 应用串口: ... @ ... (已打开)`；状态变绿 |
| 无界面配置 | `FIRECONTROL_SERIAL_1="/dev/ttyS0:115200" ./deffire-gui-dev` | 同上，且不用点界面 |
| 伪终端代替硬件 | `socat -d -d pty,raw,echo=0 pty,raw,echo=0`，另开终端 `printf '%s' '<完整帧>' > /dev/pts/M` | 那一路上报报文 + 五行更新 |
| 拔线/断线 | 打开后拔掉 USB 转串口 | 控制台 `串口断开: ...（每秒自动重连）`；状态变 `未打开：...（每秒重连）` |
| 重新插回 | 插回同一设备 | 1 秒内 `串口已重连: ...`，之后继续收帧 |
| 非法波特率 | 用环境变量填 `xxx:12345` | 报"不支持的波特率"，不崩 |

**边界**：每个信息源现在有独立的 parser/status；接口页「收到的报文」只显示当前
信息源自己的帧。每路一个独立串口设备；**不预置设备路径/波特率**。温度阈值、
蜂鸣器/LED 联动规则仍未定。

---

## 5. 证据模板（直接复制回填）

> 2026-10-18：首次验证结论为通过。下面保留模板，方便下次回归时把当次证据填进来。

```markdown
### 实测记录：首次上板 / Windows 构建
- 日期 / 执行人：
- [Win] PowerShell 版本：            ；脚本提交：
- [Win] build-armhf.ps1 退出码：      ；耗时：
  - deffire-gui-dev ：______ 字节，sha256 ______
  - deffire-dev     ：______ 字节，sha256 ______
  - TOOLCHAIN.txt   ：host_runtime = ______ ；built_local = ______
- [板] verify-on-board.sh 退出码：____ ；结论：______ ；缺库：______
  - uname -m / -r：______ ；glibc：______ ；内存：______ ；X0：有/无
- [板] 启动横幅：屏幕 ______ x ______ ；字体路径 ______ ；布局行：______
- [板] 按钮：
  LED 开 ___  LED 关 ___  LED 心跳 ___  蜂鸣器开 ___  蜂鸣器关 ___  蜂鸣器心跳 ___
  外部蜂鸣器 开/关 ___（能响吗？扫过的频率里最响的是 ____ Hz）
  信息源 1..12 ___  设置 ___  退出 ___   （通过 / 失败，失败写控制台原文）
- [板] 信息源页：串口设备 ___ / 波特率 ___；应用后模块状态 ______；【刷新串口】设备数 ______
- [板] 串口：完整帧 ______ ；不完整帧不发布 ___ ；拔线断开 ___ ；插回重连 ___ ；非法波特率 ___
  + 心跳→开 顺序回归：常亮 / 仍闪
- [板] 旁证：sys-led brightness=___ trigger=___ ；beep brightness=___ trigger=___
         gpioinfo 里 GPIOA line 6 的 consumer/direction = ______
- [板] 开蜂鸣器后主循环还跟手吗（点击延迟有没有变差）：______
- [板] 触摸坐标：右上 ___ ，左上 ___ ，左下 ___ ，右下 ___ ，中央 ___
- [板] JSON：默认 ______ ；字面量 ______ ；文件热更新 ______ ；非法输入 ______
- [板] 五行：烟雾 ___ ；火焰 ___ ；温度 ___ ；蜂鸣器 ___ ；LED ___
- [板] 完整帧：不完整时不发布 ___ ；补齐后显示 ___ ；坏帧后新帧 ___ ；收到的报文行 ______
- [板] 铺满：标题栏 有/无 ；铺满是/否 ；差多少 ______
- [板] 权限：ls -l 输出 ______ ；id -nG 含 leds：是/否
- 结论：本次回归 [ ]通过 [ ]未通过(现象) ；铺满 [ ]是 [ ]否（否 -> 按 workflow §2.2 交 DE 处理）
```

---

## 6. 看着像 bug、其实不是

| 现象 | 真相 |
|---|---|
| 点按钮打印 `失败 (开发机无此设备)` | 代码里**权限不足**与**设备不存在**共用这一条字符串。板上出现它，先查 §2.3 的权限，别急着改代码 |
| GUI 完全起不来，报 `error while loading shared libraries: libgpiod.so.2` | 板上没装运行时库：`sudo apt install libgpiod2`（见 §2.2）。**不是**传感器功能的问题 |
| 启动后点几下没反应，打印 `忽略启动瞬间的点击 (启动后 xxx ms)` | 有意为之：`kIgnoreClicksMs = 400`，挡窗口映射瞬间的杂散 ButtonPress |
| 屏幕中央大字是 `halloworld` | **有意保留**（WARNING A-3）。要改只动 `src/appgui/gui.cpp` 的 `kText` |
| 首页只有信息源按钮、找不到 LED/蜂鸣器的开关 | 界面是分页的：先点右上角【设置】，本地测试入口都在设置页里 |
| 信息源页温度/蜂鸣器/LED 显示 `--（...待...）` | 正常：温度数值和蜂鸣器/LED 联动规则还没接；烟雾/火焰默认显示 `0（正常）` |
| 切页后窗口标题栏/状态灯没变 | 有意为之：顶栏所有页一致，切页只重建底栏按钮 |
| 开了心跳后读 `brightness` 值一直在变 | trigger 生效时内核周期性覆盖 brightness（WARNING.md C-8），正常 |
| 启动横幅没有 `FreeType` 那行了 | 正常。那句"FreeType 初始化失败"原本是**误报**（`lv_init()` 已经初始化过 FreeType），已删除 |
| 开发机上中文是方框 | 字体候选表是 Debian 布局，Fedora 上一条都不存在（WARNING A-2）；板上已验证中文正常 |

---

## 7. 结果怎么回流到文档

| 结果 | 要改的地方 |
|---|---|
| 首次上板通过（2026-10-18） | 已关闭 `WARNING.md` A-1，并更新 `NOTES.md` §9「上板实测（LVGL 版）」与 §10 对应条目；后续回归仍按本表回流 |
| Windows 构建通过（2026-09-19） | 已更新 `workflow.md` §1 与 `NOTES.md` 相应说明 |
| 没铺满 | 按 [workflow.md](workflow.md) §2.2 交 DE 处理（EWMH 请求/窗口规则），**不自己实现**；把实测数据（差多少像素、有无标题栏）记下来 |
| 任一项失败 | **不要**把文档改成"已通过"。开 issue / 记进 `WARNING.md` A 节，写清现象与复现命令 |

---

## 附：Linux 侧对照命令与最近一次回归结果

同一套容器、同一份 `Dockerfile.build-armhf`，Linux 侧等价命令是：

```bash
./build.sh gui-armhf              # 构建
./build.sh gui-armhf --rebuild    # 连环境镜像一起重建
```

**2026-09-15 在 Fedora 上的回归结果**（供 Windows 侧对照"正常长什么样"）：

- `./build.sh gui-armhf --rebuild` 退出码 **0**；环境镜像 12 步全部命中缓存，未联网。
- 产出 `deffire-gui-dev` **约 0.4 MB**、`deffire-dev` **约 10 KB**；
  连跑两次 sha256 **完全相同**（`6b2c14d2…`），即当前配置下构建可复现。
  （接入 libgpiod 之前是同源 403,804 字节 / `c73d670c…`，同样是两次一致。）
- `file`：`ELF 32-bit LSB pie executable, ARM, EABI5`。
- `NEEDED`：`libX11.so.6`、`libfreetype.so.6`、**`libgpiod.so.2`**、`libstdc++.so.6`、`libgcc_s.so.1`、`libc.so.6`。
- **零警告已复核**：`./build.sh gui-armhf` 打印的是**过滤后**的输出
  （`grep -E '^\s*\[|Class:|Machine:|Flags:|error|Error|FAIL'`），所以一次正常构建
  只能证明"没有 error"，**证明不了"0 警告"**。同日另跑一次全量日志（389 行、
  318 条 `Building … object`、退出码 0）：`warning` / `error` / `FAIL` 命中 **0 条**
  → `-Wall -Wextra` 零警告的约定仍然成立。复核命令：

  ```bash
  podman build --build-arg BASE_IMAGE=firecontrol-armhf:bookworm \
      -f armhf-toolchain/Dockerfile.build-armhf -t firecontrol-armhf-build:tmp . > /tmp/full.log 2>&1
  grep -icE 'warning|error|FAIL' /tmp/full.log      # 期望 0
  podman rmi -f firecontrol-armhf-build:tmp
  ```
