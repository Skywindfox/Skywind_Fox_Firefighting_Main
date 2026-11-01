# HANDOFF — 2026-10-31 会话交接

> **这是一份快照，不是长期文档。**
> 长期状态以 `README.md` / `NOTES.md` / `WARNING.md` / `workflow.md` /
> `VERIFY-RUNBOOK.md` 为准；下次接手先读本文件，再按需查这五份。

---

## 0. 当前仓库状态

| 项 | 值 |
|---|---|
| 分支 | 只剩 `main`（本地/远端） |
| 最新提交 | `0d6b292` `docs: 明确报文头/尾归发送方(app 不添加) + 清理交接备注`（内容提交；之后只同步本表的小提交不算，hash 以 `git log --oneline` 为准） |
| `main` = `origin/main` | `0d6b292`（已推送） |
| 工作区 | 干净；`build/` 已重建（本轮），`build-armhf/` 未生成 |
| 远端 | `git@github.com:Skywindfox/Skywind_Fox_Firefighting_Main.git` |
| 最近测试 | 本轮重建后 `ctest` 3/3 通过 |

最近提交：

```text
本轮     docs: 说明报文头/尾归属（app 不添加）+ 移除交接里的临时备注
e1c4964  docs: 添加会话交接快照 HANDOFF.md
d96d1a2  docs: 更新仓库地址为 Skywind_Fox_Firefighting_Main
a4ccfd1  feat: cJSON 帧解析 + 12 路信息源接口页
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
- 信息源接口页：左侧 12 路列表、中间接收/报文卡片、右侧 GPIO/线号设置；
- 逐路 JSON 帧解析，支持不完整帧丢弃、半包拼接、坏帧后新 start 重开；
- 外部无源蜂鸣器 GPIOA:6、板载 LED/蜂鸣器 sysfs 控制仍然保留。

---

## 2. 最近的代码结构

| 文件 | 作用 |
|---|---|
| `include/json_frame.h` | `JsonFrameParser`：`{"start":1}...{"end":1}` 流式切帧 |
| `src/library/json_frame.cpp` | 半包、坏帧、新 start 重开、8 KB 上限、多帧 |
| `include/json_status.h` | 12 路 `JsonStatus`、`json_source_feed/reset`、`json_status_for` |
| `src/library/json_status.cpp` | 每路独立 parser/status；文件来源；线程安全 |
| `src/library/start.cpp` | 主循环、`status()` 里逐路打印变化、硬件动作 |
| `src/appgui/gui.cpp` | 首页/设置/调试页 + 信息源接口页全屏覆盖层 |
| `tests/test_json_frame.cpp` | 帧解析单元测试 |
| `tests/test_json_status.cpp` | JSON 解析 + 逐路 feed/reset 测试 |
| `third_party/cjson/` | cJSON 1.7.18（MIT，vendored，板上无需安装） |

CMake：

- `project(... LANGUAGES C CXX)`；
- `cjson` 静态库，`firecontrol` 私有链接；
- `ctest` 三个测试：`greeting`、`json_status`、`json_frame`。

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
中间下卡：模块状态：...
          收到的报文：...
          预留：
右侧：信息源设置
      GPIO 接口 [下拉]
      线号 [下拉]
      占位
      应用设置
```

- 覆盖层只在 `Page::Sensor` 显示，会盖住普通页顶栏/底栏；
- 顶栏【首页】关闭覆盖层；
- 左侧切换信息源时，当前项加粗，页面只显示那一路的完整帧；
- 配色：深蓝黑 `#0E141B`、面板 `#161D26`、细线 `#2A3644`、
  少量琥珀 `#F2B544`、正文 `#E8EDF2`、正常状态绿色 `#4ADE80`。

数据行当前是**默认/占位**：

| 行 | 当前值 | 状态 |
|---|---|---|
| 烟雾状态 | `0` | `正常` |
| 火焰状态 | `0` | `正常` |
| 温度状态 | `--` | `待接入` |
| 蜂鸣器状态 | `--` | `规则待定` |
| LED状态 | `--` | `规则待定` |

这些值还没有从每路 payload 里解析出来；`g_sensor_values[5]` 是值标签，
以后接字段时更新它们。

---

## 5. GPIO / 串口 / 默认值

### 已确定

- **0 = 正常，1 = 异常**（传感器模块那边定义），app 不做极性换算，
  也**没有**高有效/低有效选项。
- 每个信息源独立保存 GPIO/线号选择（`g_sensor_cfg[12]`）。
- 【应用设置】只保存选择，不读硬件；状态显示“已选择 GPIOx:y（等待数据接入）”。
- 线号随端口动态变化：GPIOH 15、GPIOI 8、其余 16。
- 默认映射表在 `gui.cpp` 的 `kDefaultSensorConfig[12]`，**现在全是 `{-1,-1}`**。
  等 12 组实际默认映射提供后填入；填了有效值启动即显示“已选择”。
- **没有猜任何串口设备路径/波特率**，也没有硬编码 GPIO 默认接线。

### 未完成

- **模块报文的实际字节**：头/尾究竟是什么（现在写死 `{"start":1}` / `{"end":1}`），
  以及传输方式是 GPIO 开关量（0/1，`gpio_read_value()`）还是字节流（串口/其它总线）
  —— 接口页右栏选的是"端口 + 线号"，而 `json_source_feed()` 收的是 JSON 字节流，
  两者之间**还缺一层**；`json_source_feed()` 至今**没有调用点**（【应用设置】只保存选择）；
  若模块是"查询-应答"式，还需要 app 自己发请求报文（现在没有任何发送代码）；
- 12 路真实串口（确认走字节流之后）：设备路径、波特率、termios 读取线程、断线重连策略；
- 串口线程收到字节后调 `json_source_feed(index, data, len)`；
- 断线重连调 `json_source_reset(index)`；
- 12 组默认 GPIO 映射；
- payload 字段定义：目前只解析 `data` / `smoke`，UI 五行没有对应字段；
- 温度阈值/单位；
- 蜂鸣器/LED 联动规则（现在都是 `--（规则待定）`）；
- 上板验证新的接口页/逐路帧（之前的板上验证是旧 UI）。

---

## 6. 构建 / 运行 / 部署

构建目录已删除，先重新构建：

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

单路完整帧示例：

```json
{"start":1}{"data":7,"smoke":1}{"end":1}
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

1. ~~重新构建并跑一次 `ctest`~~（本轮已做：清空构建目录后重建，3/3 通过）；
2. 向硬件/模块侧确认三件事：报文头/尾的实际字节、传输方式（字节流还是开关量）、
   模块是纯上报还是"查询-应答"（后者要 app 发请求报文）；
3. 填 12 组默认 GPIO 映射到 `kDefaultSensorConfig`；
4. 确定串口设备/波特率，加读取线程，调 `json_source_feed(index, ...)`；
5. 定义 payload 字段（烟雾/火焰/温度…），把 `g_sensor_values[5]` 接上；
6. 定温度阈值和蜂鸣器/LED 联动规则；
7. 上板验证接口页、逐路串口、半包/坏帧/重连；
8. 更新 `VERIFY-RUNBOOK.md` 的实测记录。
