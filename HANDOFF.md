# HANDOFF — 2026-10-31 会话交接

> **这是一份快照，不是长期文档。**
> 长期状态以 `README.md` / `NOTES.md` / `WARNING.md` / `workflow.md` /
> `VERIFY-RUNBOOK.md` 为准；下次接手先读本文件，再按需查这五份。

---

## 0. 当前仓库状态

| 项 | 值 |
|---|---|
| 分支 | 只剩 `main`（本地/远端） |
| 最新提交 | `96d4e18` `feat: 每路独立串口来源 + 模块字段解析`（内容提交；之后只同步本表的小提交不算，hash 以 `git log --oneline` 为准） |
| `main` = `origin/main` | `96d4e18`（已推送） |
| 工作区 | 干净；`build/`、`build-armhf/` 都在（本轮 native + armhf 都重建过） |
| 远端 | `git@github.com:Skywindfox/Skywind_Fox_Firefighting_Main.git` |
| 最近测试 | 本轮 `ctest` 4/4 通过；armhf 交叉编译通过（`build-armhf/deffire-gui-dev`，约 486 KB） |

最近提交：

```text
96d4e18  feat: 每路独立串口来源 + 模块字段解析
ee59410  docs(HANDOFF): 说明表格里的 hash 以 git log 为准
06250e6  docs(HANDOFF): 同步本轮提交 hash 与推送状态
0d6b292  docs: 明确报文头/尾归发送方(app 不添加) + 清理交接备注
e1c4964  docs: 添加会话交接快照 HANDOFF.md
```

---

## 1. 项目是什么

- 智能楼宇消防系统的**本地控制端**原型，STM32MP135（正点原子 ATK-DLMP135）。
- 板子：Cortex-A7 单核 1 GHz、437 MiB 内存、Debian 12 armv7l、1024×600 触摸屏、
  Xorg `:0` + LXDE / Openbox。
- 开发机交叉编译 armhf，产物拷到板上；板上不编译。
- 两人合作：Skywindfox（平台/系统/CMake/中控/文档），FBI-0537（armhf 工具链/脚本/LVGL 重构）。

当前能力：

- 首页 12 个信息源入口；
- 设置页收纳 LED / 蜂鸣器 / 外部无源蜂鸣器本地测试；
- 信息源接口页：左侧 12 路列表、中间接收/报文卡片、右侧串口设备/波特率设置；
- **每路一个独立串口**（字节流），后台 poll 线程读取，断线每秒自动重连；
- 逐路 JSON 帧解析（不完整帧丢弃、半包拼接、坏帧后新 start 重开），payload 认模块固件的
  `temperature` / `fire_state` / `smoke_state` / `buzzer_state` / `led_state`
  （兼容旧名 `*_alarm` 与旧 `data`/`smoke`）；
- 外部无源蜂鸣器 GPIOA:6、板载 LED/蜂鸣器 sysfs 控制仍然保留。

---

## 2. 最近的代码结构

| 文件 | 作用 |
|---|---|
| `include/json_frame.h` | `JsonFrameParser`：`{"start":1}...{"end":1}` 流式切帧 |
| `src/library/json_frame.cpp` | 半包、坏帧、新 start 重开、8 KB 上限、多帧 |
| `include/json_status.h` | 12 路 `JsonStatus`（含 4 个新字段）、`json_source_feed/reset` |
| `src/library/json_status.cpp` | 每路独立 parser/status；字段解析；文件来源；线程安全 |
| `include/serial_source.h` | 串口来源：设备枚举、波特率清单、配置/状态、start/stop、env |
| `src/library/serial_source.cpp` | termios 打开、单 poll 线程、断线每秒重连、喂帧 |
| `src/library/start.cpp` | 主循环、`status()` 里逐路打印、串口生命周期、硬件动作 |
| `src/appgui/gui.cpp` | 首页/设置/调试页 + 信息源接口页全屏覆盖层（串口选择 + 五行数据） |
| `tests/test_json_frame.cpp` | 帧解析单元测试 |
| `tests/test_json_status.cpp` | 字段解析 + 逐路 feed/reset 测试 |
| `tests/test_serial_source.cpp` | 伪终端串口测试（打开/帧/断线/非法波特率/env） |
| `third_party/cjson/` | cJSON 1.7.18（MIT，vendored，板上无需安装） |

CMake：

- `project(... LANGUAGES C CXX)`；
- `cjson` 静态库，`firecontrol` 私有链接；
- `ctest` 四个测试：`greeting`、`json_status`、`json_frame`、`serial_source`。

---

## 3. JSON 帧协议与鲁棒性

传感器端定义：

```text
{"start":1} ...数据... {"end":1}
```

**报文头/尾归发送方（传感器模块），app 不添加**：`JsonFrameParser` 只做接收侧切帧
（按精确字节找标记、start 之前的字节丢弃），从不写标记；app 里唯一自己产出的成品帧是
内置示例帧 `kSampleFrame`（没配置来源时的占位，不是协议）。所以"模块自己已经带了报文
头/尾"**不需要我们再补一步**，只需要解析器的标记字节与模块实际发送的一致 —— 这两个值
现在写死为 `{"start":1}` / `{"end":1}`，尚未与模块实测比对（见 §5 未完成第 1 条）。

- 标记按**精确字节**匹配；payload 字符串里不要出现这两个字面量。
- `feed()` 可以分块；start/end 被拆到两个 chunk 也能拼上。
- 没看到 start 之前的数据丢弃。
- 看到 start 后如果又出现新 start（旧帧没 end）：**丢弃旧帧，从新 start 重开**。
- 缺 end 的帧保留在缓冲里，等后续补齐。
- 单帧超过 8 KB 丢弃，等下一个 start。
- 一次 feed 可以切出多个完整帧。
- 旧格式 `{"data":1,"smoke":1}`（无 start/end）仍然按整段 JSON 兼容。

payload 字段（模块固件的变量名，2026-11 用户给出）：

```json
{"temperature":36.5,"fire_state":1,"smoke_state":0,"buzzer_state":1,"led_state":3}
```

| 字段 | 模块语义 |
|---|---|
| `temperature` | 温度（浮点；单位由模块定，app 原样显示） |
| `fire_state` | 0 无火 / 1 有火 |
| `smoke_state` | 0 无烟 / 1 有烟 |
| `buzzer_state` | 0 灭 / 1 急促（100ms 响停）/ 2 缓慢（500ms）/ 3 长响 |
| `led_state` | 三色灯：0 灭、1 蓝闪(烟)、2 绿闪(温)、3 红闪(火)、4 蓝绿(烟+温)、5 蓝红(烟+火)、6 绿红(温+火)、7 红绿蓝轮转(三路全报) |

- 全是数字；蜂鸣器/LED 是**模块侧状态机**，app 只显示，不重算、不驱动本机蜂鸣器/LED；
- 字段**可选**：缺的不算错（`has_xxx=false`）；一个已知字段都没有才失败
  （`error="没有已知字段"`）；
- 字段存在但不是数字 → `valid=false`，`error` 指明字段名（如 "temperature 不是数字"）；
- 旧名 `fire_alarm` / `smoker_alarm` / `buzzer_alarm` 作**别名**仍认；
  旧的 `data` / `smoke` 也保留兼容。

文件来源（测试用）：

- 信息源 1：`FIRECONTROL_JSON` / `FIRECONTROL_JSON_FILE` / `./firecontrol.json` / 内置帧；
- 信息源 N（2..12）：`FIRECONTROL_JSON_FILE_<N>`；
- 文件不变不重复喂；追加只喂新增后缀；重写重置解析器；
- 之前在帧模式、重写后没有 start → 保留上一帧，不把半截当新帧；
- 读文件失败保留上一帧，只更新错误状态。

每路接口（接串口时用）：

```cpp
json_source_feed(index, data, len); // index 0..11, 线程安全, 返回切出的完整帧数
json_source_reset(index);           // 断线重连清半帧/状态
JsonStatus json_status_for(index);  // 取某一路状态（拷贝）
JsonStatus json_status();           // 信息源 0，普通页 JSON 行用
```

---

## 4. 信息源接口页（当前 UI）

入口：首页点【信息源 1..12】→ 全屏覆盖层。

```text
顶栏：火灾控制终端 | 运行中      [ 首页 ]      [ 退出应用 ]
左侧：信息源 1..12（可滚动，当前项琥珀描边）
中间上卡：接收：
          烟雾状态 / 火焰状态 / 温度状态 / 蜂鸣器状态 / LED状态
          每行三列：名称 / 值 / 状态
中间下卡：模块状态：...（串口真实状态）
          收到的报文：...
          预留：
右侧：信息源设置
      串口设备 [下拉：运行时枚举 /dev/tty*，首项"未选择"]
      波特率   [下拉：9600..921600，首项"未选择"]
      [ 刷新串口 ]      ← 原来预留的"占位"按钮
      [ 应用设置 ]
```

- 覆盖层只在 `Page::Sensor` 显示，会盖住普通页顶栏/底栏；
- 顶栏【首页】关闭覆盖层；
- 左侧切换信息源时，当前项加粗，页面只显示那一路的完整帧；
- 【应用设置】才真正 `serial_configure(index, device, baud)`：两个都选了才打开；
  有一个没选 = 取消配置并关端口（不猜默认值）；
- 模块状态：`未选择串口` / `已打开，等待数据` / `未打开：<原因>（每秒重连）`；
- 配色：深蓝黑 `#0E141B`、面板 `#161D26`、细线 `#2A3644`、
  少量琥珀 `#F2B544`、正文 `#E8EDF2`、正常状态绿色 `#4ADE80`。

接收卡五行接的是 payload 真实字段：

| 行 | 字段 | 显示 |
|---|---|---|
| 烟雾状态 | `smoke_state` | 值 `0`/`1` + `无烟`/`有烟` |
| 火焰状态 | `fire_state` | 值 `0`/`1` + `无火`/`有火` |
| 温度状态 | `temperature` | 数值（`%.1f`）+ `阈值待定` |
| 蜂鸣器状态 | `buzzer_state` | 值 `0..3` + `灭`/`急促`/`缓慢`/`长响` |
| LED状态 | `led_state` | 值 `0..7` + `灭`/`蓝闪`/`绿闪`/`红闪`/`蓝绿`/`蓝红`/`绿红`/`轮转` |

缺字段的行显示 `--（待接入）`，不拿上一帧的旧值冒充当前值；温度只给数值 +
"阈值待定"，不擅自判正常/异常。`g_sensor_values[5]` / `g_sensor_states[5]` 是
值/状态标签，由 `refreshSensorData()` 按每路 `revision` 刷新。

---

## 5. 串口 / 默认值

### 已确定（2026-11）

- 传输方式：**字节流**，每路信息源一个独立串口设备。
- **0 = 正常，1 = 异常**（模块定义），app 不做极性换算，也**没有**高有效/低有效选项。
- payload 字段：`temperature` / `fire_state` / `smoke_state` / `buzzer_state` / `led_state`
  （全数字，**可选**，缺字段不算错）；旧名 `*_alarm` 与旧 `data`/`smoke` 仍然认。
  蜂鸣器/LED 是模块侧状态机，app 只显示。
- 设备清单**运行时枚举**（`serial_list_devices()` 扫 `/dev/tty*`），波特率标准清单
  （9600..921600）；**不预置任何设备路径/波特率**，也不猜 12 路默认映射。
- 断线（EOF / 读错误）：关端口、记错误、`json_source_reset()`、**每秒重连**；
  重开成功后再 reset 一次（旧流半帧作废）。
- **只接收、不应答**：没有任何向模块发送报文的代码。
- 信息源页原来的"GPIO 接口 / 线号"下拉和 `kDefaultSensorConfig[12]` **已删除**
  —— 字节流不需要；开关量输入那条路（`kGpioInputs`）仍是空表，与接口页无关。

### 未完成

- **报文头/尾的实际字节**：代码写死 `{"start":1}` / `{"end":1}`，尚未与模块实测比对；
  对不上会静默"暂无"（不报错），接模块前必须先确认；
- 12 路接线：设备路径/波特率（用 `FIRECONTROL_SERIAL_<N>` 或界面选）、板上 `dialout`
  权限、端口是否被 getty 占用；
- 上板验证：一路跑通、半包/坏帧/拔线重连、界面模块状态是否跟着变；
- 温度阈值/单位；
- 要不要用模块给的 `buzzer_state` / `led_state` 去驱动本机蜂鸣器/LED（现在只显示）；
- "打开了但长时间没有帧" / 格式错误怎么显示，还没定。

---

## 6. 构建 / 运行 / 部署

构建目录：`build/`、`build-armhf/` 都已重建（本轮 native `ctest` 4/4 + armhf 交叉编译）。

```bash
./build.sh native        # build/ + ctest
./build.sh gui-armhf     # 交叉编译 GUI，产物 build-armhf/deffire-gui-dev
```

本地窗口：

```bash
cd FireControlApp
DISPLAY=:0 ./build/deffire-gui-dev --windowed
```

逐路文件测试：

```bash
FIRECONTROL_JSON_FILE_2=/tmp/source2.json DISPLAY=:0 ./build/deffire-gui-dev --windowed
```

串口测试（开发机上用 socat 造伪终端，板上直接填设备路径）：

```bash
socat -d -d pty,raw,echo=0 pty,raw,echo=0      # 得到两条 /dev/pts/N, M
FIRECONTROL_SERIAL_1=/dev/pts/N:115200 ./build/deffire-gui-dev --windowed
printf '%s' '<下面那条帧>' > /dev/pts/M
```

（界面上也可以选：信息源页右栏 串口设备 + 波特率 → 【应用设置】；【刷新串口】重扫。）

单路完整帧示例：

```json
{"start":1}{"temperature":36.5,"fire_state":1,"smoke_state":0,"buzzer_state":1,"led_state":3}{"end":1}
```

上板：

```bash
./armhf-toolchain/verify-on-board.sh fbi@<板子IP>
./armhf-toolchain/deploy-to-board.sh --run fbi@<板子IP>
```

板上不需要装 cJSON；仍需要已有的 `libX11`、`libfreetype`、`libgpiod2`、中文字体。

---

## 7. 文档与协作约定

- `README.md`：目的/意义/任务；
- `NOTES.md`：环境、构建、部署、界面、实测、待办；
- `WARNING.md`：踩坑与已知问题；
- `workflow.md`：未决取舍、界面约定、协作硬约束；
- `VERIFY-RUNBOOK.md`：上板验收；
- `third_party/cjson/README.md`：cJSON 版本/来源/升级方式。

关键约定：

- `Action::Nothing` 必须留在枚举第一位；
- `onAction()` 的 `switch` 不写 `default`；
- 加按钮/页面优先改表（`kHomeButtons` / `kSettingsButtons` / `kPages`），
  数组长度用 `countOf()`；
- 切页只置 `g_page_dirty`，重建在主循环里做；
- 控制台 `点击【x】坐标 (x,y) → 结果` 格式不要改；
- 测试以 `VERIFY-RUNBOOK` 手工验收为主；`ctest` 只覆盖纯逻辑层；
- 不预置猜出来的传感器名/GPIO/串口配置。

---

## 8. 环境注意

- 远端仓库已从 `DefFireWork` 改名为
  `Skywind_Fox_Firefighting_Main`，remote 已更新。
- 这台机器 SSH 偶尔报
  `/etc/ssh/ssh_config.d/20-systemd-ssh-proxy.conf` 权限问题，
  git 可以这样绕过：

  ```bash
  GIT_SSH_COMMAND='ssh -F /dev/null -o StrictHostKeyChecking=accept-new' \
      git push origin main
  ```

- 不要直接推 PR；此前已确定直接推 `main`。
- 当前没有 `gh` / GitHub token，无法自动开 PR。
- UI 本地预览时注意别把合成点击发到别的窗口（桌面同时有 QQ/WPS 等）。

---

## 9. 下一步建议顺序

1. **板上接一路真串口**：确认设备路径/波特率（`ls /dev/tty*`、`sudo gpioinfo` 无关；
   串口看 `ls -l /dev/tty*` 与 `dmesg`），用户是否在 `dialout` 组，端口有没有被 getty
   占用；用 `FIRECONTROL_SERIAL_1` 或界面选完点【应用设置】；
2. 验证半包 / 坏帧 / 拔线重连，以及界面"模块状态"是否跟着变（已实现每秒重连）；
3. 和模块端核对**报文头/尾的确切字节**（现在写死 `{"start":1}` / `{"end":1}`）；
4. 现场确定 12 路各自的串口设备 + 波特率，决定要不要写死默认值（现在不预置）；
5. 定温度阈值/单位；
6. 定蜂鸣器/LED 联动规则（LED 目前没有 payload 字段）；
7. 补"长时间无帧/格式错误"的显示规则；
8. 更新 `VERIFY-RUNBOOK.md` 的实测记录（帧示例已改成新字段）。
