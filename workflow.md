# workflow.md — 项目活文档

> 这份文件接替已删除的 `HANDOFF.md`。
> 区别在**时效**：`HANDOFF.md` 是一次性快照（写的时候对应某个提交，几天就过期）；
> 这里只放**不会随提交过期的东西** —— 未决的取舍、界面的约定、以及"真机验证到哪了"。

| 想知道什么 | 去哪 |
|---|---|
| 项目的目的、意义、要做的事 | [README.md](README.md) |
| 怎么构建 / 怎么跑 / 板上什么环境 | [NOTES.md](NOTES.md) §1–§4 |
| 踩过的坑、已知问题、未验证项 | [WARNING.md](WARNING.md) |
| 要在 Windows 上构建，或者要上板实测 | [VERIFY-RUNBOOK.md](VERIFY-RUNBOOK.md) |
| 未决设计问题、界面约定、协作上的硬约束 | **本文** |

---

## 1. 真机验证进度

> 更新这一节时**只写实测过的**。没跑过的写"还没做"，不要写"应该没问题"。

| 项 | 状态 | 说明 |
|---|---|---|
| Linux 交叉编译（`build-armhf.sh`） | ✅ 实测通过 | 0 警告；同源同镜像重建产物 sha256 逐字节一致（可复现） |
| Windows 交叉编译（`build-armhf.ps1`） | ✅ 已跑通 | FBI-0537 在 Windows 上实跑（2026-09-19，PR #8）：`-Wall -Wextra` 零警告，产物约 0.4 MB / 10 KB（具体字节以构建产物为准）。**原待办 #2 已达成**（此前只改过、从没执行过） |
| 板上实测（LVGL 版） | ✅ 已上板验证通过 | 2026-10-18 项目成员确认；判据见 runbook §3–§4，具体日志/证据建议回填 §5 |
| 原生 GUI（开发机预览） | ✅ 六类页面正常 | 首页（12 个信息源）/ 设置 / 信息源 / LED / 蜂鸣器 / 外部蜂鸣器：切页、按钮派发、退出路径都实测过；JSON 文件热更新也实测过 |
| 骨架重构（四阶段 + 单循环） | ✅ 原生实测 | 0 警告；主循环 910 次/秒（与重构前一致）；点【退出】→ `gui_teardown()` → 进程干净退出；`--help` 不崩。见 §2.1 |
| GPIO 输入（libgpiod） | ⚠ 只有骨架 | 依赖链已配齐并交叉编译通过；**没有任何调用点**，板端也没验过（不知道开关量传感器接在哪个 gpiochip / 哪条线上）。**信息源页已改用串口字节流**，这条路留给将来真正的开关量输入，见 §2.3 |
| JSON 解析（cJSON） | ✅ 原生构建 + 单测 | `temperature` / `fire_state` / `smoke_state` / `buzzer_state` / `led_state`（旧名 `*_alarm` 作别名、旧 `data`/`smoke` 兼容）；`test_json_status` 覆盖字段/类型/语法/空对象；字面量/文件来源已在控制台验证。见 §2.5 |
| 串口来源 | ✅ 原生构建 + 单测（伪终端） | `serial_source.cpp`：每路设备+波特率、poll 线程、断线每秒重连；`test_serial_source` 覆盖打开/帧/断线/非法波特率。**板上真串口待接**，见 §2.6 |
| 全屏 | ✅ 方案已定（交给 DE） | 不自己实现；按 EWMH 向 LXDE/Openbox 提交 `_NET_WM_STATE_FULLSCREEN` 请求（或配窗口规则），见 §2.2 |
| 测试策略 | ✅ 手工验收 | 硬件 / GUI / 触摸以 VERIFY-RUNBOOK 为准；`ctest` 只覆盖纯逻辑层（当前 `greeting` / `json_status` / `json_stream` / `serial_source`） |

---

## 2. 未决的设计问题

### 2.1 业务层与 GUI 的接线（✅ 已解决 —— 2026-09-19 重构）

**原状**（重构前）：`gui.cpp` 自带 `main()` 和自己的 LVGL 循环，`start.cpp` 的
`status()` / `render()` 在 GUI 里是死代码，GUI 里也没有周期业务逻辑的位置。

**方向选的是 B**（`cpp_start` 接管循环，GUI 不再有自己的 `main()`），现在的结构：

```
main.cpp（收命令行参数 + 定义程序级全局量 g_quit / g_argc / g_argv）
   └─ cpp_start()                        ★ 唯一主循环
        device_init()                    ← 硬件/配置，一次
        gui_build()                      ← 建界面，一次
        while (!g_quit) { status(); render(); }
        gui_teardown()                   ← 收尾
```

- `gui.cpp` 原来的 `main()` 拆成三段：`gui_build()`（参数解析 → 探 X → `lv_init` → 字体
  → 窗口 → 输入 → 布局）、`render()`（切页落地 + 一次 `lv_timer_handler()`）、
  `gui_teardown()`（停蜂鸣器 → 删显示 → 删字体 → `lv_deinit`，用 `g_lv_inited` 兜住
  "界面没建起来"的情形）。
- 控制台版换 `src/appgui/gui_stub.cpp` 的同一组实现（`render()` 置 `g_quit`，循环只转一圈），
  于是 `main.cpp` / `cpp_start()` 只有一份，**不用 `#ifdef`**。
- **周期逻辑的位置定死了：`status()`**，内部 200 ms 节流（主循环约 1 ms 一轮，传感器不需要这个频率）。
  不跟 UI 同频的东西（方波）自己开线程，不要塞进 `render()` —— 那样会跟帧率绑死。
- **`status()` 的日志按"变化"触发**：采样照做，但只有状态签名变了才打印一行。
  否则 5 Hz 的整块日志会把终端刷满（重构后第一次跑就撞上了），而没变化的信息
  本来也没价值。新加模块请沿用这个约定：**采样式函数不要每轮 printf**。
- **实测（原生）**：主循环 910 次/秒，与重构前一致；点【退出】→ 循环退出 →
  `gui_teardown()` 打印"退出" → 进程 0 退出；`--help` 也在 `gui_build()` 里提前返回，不崩。
- 注意：`g_quit` 等符号由 `main.cpp` 定义，而 `firecontrol` 静态库里引用它们 ——
  链接没问题（每个产物各编一份 `main.cpp`），但**别在库里新开对 `start.cpp` 的依赖**。

### 2.2 真全屏（2026-10-18：交给 DE，不自己实现）

板上与开发机都是 LXDE/X11（板上 WM 为 Openbox），窗口会被接管、加装饰。
**决定：不再自己实现真全屏** —— 全屏交给 DE：应用侧按 EWMH 向 WM 发送
`_NET_WM_STATE_FULLSCREEN` 请求（或在 LXDE/Openbox 里配窗口规则），是否铺满由 WM
决定；不在 GUI 里手工算坐标、也不造无边框窗口。

**当前状态（2026-11 已落地）**：`gui.cpp` 在**主循环首帧之后**（`ensureFullscreen()`）
按 EWMH 发 `_NET_WM_STATE_FULLSCREEN`，最多重试 8 次、间隔 250 ms；`--windowed` 预览
模式不发。为什么不能在建窗后立刻发：窗口要等 LVGL flush 映射请求、WM 处理完才存在，
早发会被 WM 忽略（实测：请求发早了窗口只会被"最大化"）。

请求只是**请求**：是否铺满仍由 WM 决定。如果板上 Openbox 不认，走 DE 侧配置
（lxpanel 自动隐藏 / Openbox per-app 规则），见 WARNING C-7。

### 2.3 传感器模块：开关量输入（libgpiod，暂存）

**传输方式已定（2026-11）：模块走串口字节流，见 §2.6。** 这一节讲的是另一条通道：
接在 GPIO 上的**开关量**输入（0/1 电平），给"只有电平、没有报文"的探测器预留。
现在代码量还是 0。`useable_tools::gpio_read_value()`（libgpiod v1）依赖链已配齐 ——
`armhf-toolchain/Dockerfile` 里装了 `libgpiod-dev:armhf`（1.6.3，v1），
CMake 用 `WITH_GPIOD`（默认 AUTO）控制，见 NOTES.md §6。

> 信息源页右栏原来的"GPIO 接口 / 线号"下拉已经去掉（那是开关量接法，和字节流不是一回事）；
> 页面现在选的是"串口设备 + 波特率"。`kGpioInputs` 仍是空表，等真有开关量探测器再接。

**只支持 v1 API**：`gpiod_chip_open_by_label` / `gpiod_line_request_input` 这套在
**v2 里被删了**。容器与板子（Debian 12）是 1.6.3 = v1，可用；**Fedora 44 是 2.2.5 = v2**，
所以原生 Fedora 构建拿不到这个功能（CMake 会认出版本不符并明确跳过，不会拿一堆
"未声明标识符"糊你一脸）。要在原生环境用它，得先把 `gpio_read_value()` 移植到 v2。

**板上实测（2026-09-19，D-9 修好权限之后，`gpiodetect` / `gpioinfo` 不再 Permission denied）**：

控制器 9 个。`chip_label` 就是 `gpiodetect` **方括号里**那个 —— **`GPIOA` … `GPIOI`**：

| gpiochip | label | 线数 | 备注（实测快照） |
|---|---|---|---|
| 0 / 1 / 2 / 3 | GPIOA / GPIOB / GPIOC / GPIOD | 16 | 绝大多数线被内核占用 |
| **5** | **GPIOF** | 16 | **line 8 = `"beep"`（蜂鸣器）**；line 14 = `"USER-KEY1"`（板上按键） |
| 6 / 7 | GPIOG / GPIOH | 16 / 15 | GPIOH line 5 = `"reset"` |
| **8** | **GPIOI** | 8 | **line 3 = `"sys-led"`（LED）**；0 = `irq`、1 = `spi0 CS0`、2 = `reset` |

⚠ **别把 `gpiochip0` 当 label 填。** 那是**设备名**（`gpiod_chip_open_by_name()` 用的那个），
而 `gpio_read_value()` 走的是 `gpiod_chip_open_by_label()` —— 填 `gpiochip0` 会找不到芯片。

**"LED / 蜂鸣器的引脚归内核持有"现在是实测结论，不是推断了**：`gpioinfo` 里那两条线的
consumer 就是 `"sys-led"` / `"beep"`，状态 `[used]`。所以它们**必须**继续走 sysfs，
用 libgpiod 去 request 会失败（EBUSY）。两者 DT 极性都是 **active-low**，但这不影响我们
写 sysfs（`brightness=1` 就是"亮 / 响"）。

**传感器接在哪条线上，仍是硬件事实 —— `gpioinfo` 给不了。** 它只能告诉你"哪些线空闲"，
而"空闲"不等于"接了什么"。所以 `src/library/start.cpp` 的接线表保持空白，等接线确认后一行填一行。
下面是当天的空闲线快照（仅供对照，**别拿它当接线依据**）：

```
GPIOA: 6, 11, 13(output), 14      GPIOB: 10          GPIOC: 14, 15
GPIOF: 15                         GPIOH: 0, 1, 4(output)      GPIOI: 4, 5, 6, 7
GPIOD / GPIOE / GPIOG: 全部被占用
```

板端还要有运行时库：`sudo apt install libgpiod2`。**不装整个界面起不来**（不是局部功能失效），
因为交叉产物链着 `libgpiod.so.2`；`verify-on-board.sh` 会把缺失的库列出来。

**后续方向（2026-10-18）：做成可扩展的传感器注册表。** 现在不知道最终会接几个、什么型号的
探测器，所以 `kGpioInputs` 这个静态数组**不是最终设计**。后面改成配置文件或运行时注册：
一路探测器一条记录（名称 / chip_label / line / consumer / 极性 / 去抖等），支持任意 N 路；
新增探测器不改业务代码，最好也不用重新编译。当前先保持空表 —— 不预置猜出来的接线。

### 2.4 GPIO **输出**（外部无源蜂鸣器，2026-09-19 随 PR #8 加的）

板上 PA6（= **GPIOA line 6**，实测空闲）接了一个**无源**蜂鸣器模块，设置页有入口、
独立调试页是 开 / 关 / 返回（**没有"心跳"** —— 它不归内核 leds-gpio 管，没有 `trigger` 文件）。

写 GPIO 有两条路，**别用错**：

| 用途 | 用什么 |
|---|---|
| 偶尔写一次电平 | `useable_tools::gpio_write_value()` —— 内部走一整轮 open/request/release/close |
| **反复翻转**（方波、PWM 模拟） | `useable_tools::gpio_output_open()` 拿手柄，循环里只 `gpio_output_set()`，用完 `gpio_output_close()` |

**教训（PR #8 里出现过的写法）**：每个脉冲都调 `gpio_write_value()`，而主循环每轮跑一次
（~900 次/秒）⇒ ~1800 次/秒的芯片开关，板上单核会被拖慢；而且脉冲之间线是**放开**的，
引脚没有持续驱动 —— 现象就是"一连串咔哒声，不是音调"。

**发声不能挂在主循环上**：挂在上面时每轮最多翻一次电平，音调 = 循环速率/2（开发机 ≈450 Hz，
板上更慢 —— 实测"音调很低"）。现在 `buzzer_out_set(true/false)` 只置一个 `atomic` 标志，
**专用线程**用 `clock_nanosleep` 的**绝对时刻**（`TIMER_ABSTIME`）翻转 GPIOA:6，频率与
主循环彻底无关。频率默认 **2 kHz**，可在板上运行时试：

```bash
FIRECONTROL_BUZZER_HZ=3000 DISPLAY=:0 ./deffire-gui-dev     # 100..8000，超范围会提示并回落
```

试出这个蜂鸣器最响的频率后，把 `kOutBuzzerDefaultHz` 改成它（小无源蜂鸣器的谐振点通常在
2~4 kHz）。**开发机上实测**：默认 2 kHz 时线程每秒约 4028 次翻转（≈2014 Hz），主循环仍
909 次/秒不受影响；线程占单核约 3%（那是"写值是空操作"时的开销，板上每次多一次 ioctl）。

想要**零 CPU**、更准的音调仍可走内核 PWM（`/sys/class/pwm`），但那要把 PA6 复用成 PWM
通道并上板验证 —— 当前先用 GPIO + 线程这套。

**收尾**：界面退出前调 `buzzer_out_shutdown()` —— 停声、把 PA6 还给系统、并
`pthread_join` 掉发声线程（不是丢一个 detached 野线程在那儿）；之后再
`buzzer_out_set(true)` 仍能重新起来。用一个小测试程序验证过：两次开关都能 join + 释放、
退出码 0。

**还没实测**：板上那个蜂鸣器"能响"已确认，但**最响的频率**没试过（用上面的环境变量扫一遍）。

**一个副作用（知道就好，暂时不用管）**：`gpio_read_value()` 与 `write_File()` 在同一个
翻译单元（`usetools.cpp`），链接器为 `write_File` 拉进这个 `.o` 时会把 gpiod 引用一起带上
—— 于是**连不读 GPIO 的控制台程序 `deffire-dev` 也依赖 `libgpiod.so.2`**
（产物从约 5.6 KB 变成约 10 KB，具体以构建产物为准）。想去掉这个连带依赖，把 `gpio_read_value()` 挪到自己的
`.cpp`（如 `src/library/gpio.cpp`）并登记进 `CMakeLists.txt` 即可，那样只有真正调用它的目标才链
libgpiod。**现在没做** —— 板子无论如何都要装 `libgpiod2`（GUI 需要），收益只是让控制台
程序少一个依赖。

`~/下载/` 里那份官方 `main.c` 用的是"综合例程扩展板"的 GPIO 配置（3 个 LED、蜂鸣器走
`EV_SND`），**和手上这块底板不是一回事** —— 可以看它怎么调 libgpiod，但引脚编号别照抄。

### 2.5 报文格式与解析（2026-11 适配探测版）

引入了 **cJSON 1.7.18**（`third_party/cjson`，MIT，随仓库提交，板上不需要另装
`libcjson-dev`）。探测版模块发的是**带信封的一整条 JSON**，结尾 `\r\n`：

```text
{"header":"SENSOR_DATA","data":{"temperature":20.06,"int_dev":1,"int_err":0,
 "fire_alarm":1,"smoke_alarm":0,"led_state":0,"buzzer_state":0},"footer":"END"}\r\n
```

- **不再要求 `{"start":1}` / `{"end":1}`**（2026-11 删除）：`src/library/json_stream.cpp`
  从字节流里抠出 `{ ... }` 完整对象（花括号平衡算一条），对象之外的字节 —— 模块自己的
  头/尾、`\r\n`、垃圾 —— 全部丢弃。分块到达、多条对象、字符串/转义/嵌套都能处理，
  单条上限 8 KB。
- 状态字段在 **`data` 子对象**里：有 `data` 对象就只从里面取，没有则按旧扁平格式从
  根对象取。`header` / `footer` / `int_dev` / `int_err` 忽略。
- 认 5 个状态字段：`temperature`（浮点）、`fire_alarm`（0 无火/1 有火）、
  `smoke_alarm`（0 无烟/1 有烟）、`buzzer_state`（0 灭/1 急促/2 缓慢/3 长响）、
  `led_state`（0 灭 … 7 红绿蓝轮转）。旧名 `fire_state` / `smoke_state` /
  `smoker_alarm` / `buzzer_alarm` 作别名，旧的 `data`/`smoke` 也认。
  蜂鸣器/LED 是模块自己驱动的状态机，app 只显示，不驱动本机硬件。
- **字段全部可选**：缺字段不算错（对应 `has_xxx=false`），一个已知字段都没有才失败
  （`error="没有已知字段"`）；字段存在但不是数字才失败（如"temperature 不是数字"）。
- 如果模块仍然发 `{"start":1}` / `{"end":1}`：它们也是合法对象，但没有已知字段，
  会被**静默跳过**（不会在界面报“未识别”）。
- 数据来源优先级：`FIRECONTROL_JSON` 字面量 > `FIRECONTROL_JSON_FILE` 文件 >
  当前目录 `firecontrol.json`（存在才用）> 内置示例报文 > **串口**（§2.6，真实来源）。
- 文件来源每 200 ms 检查一次：内容没变不重复喂；追加只喂新增后缀；重写则重置切分器。
- 每个信息源有独立的 `JsonObjectSplitter` / `JsonStatus`；接口页「收到的报文」只显示
  当前信息源的对象原文。串口线程调 `json_source_feed(index, data, len)`，index=0..11；
  断线重连调 `json_source_reset(index)`。
- **报文头/尾归发送方（模块），app 只解析、不添加**：切分器从不写标记，app 自己唯一的
  成品报文是没配置来源时的内置示例（占位，不是协议）。因为只认 `{...}`，**不需要再和
  模块核对任何标记字节**。
- 测试：`tests/test_json_status.cpp` + `tests/test_json_stream.cpp`；原生 `ctest` 4/4
  （含 §2.6 的串口测试）。
- **还没做**报警判定/联动：报文进得来、五行填得上；但没有根据 `fire_alarm` /
  `smoke_alarm` / `temperature` 驱动本机蜂鸣器/LED（模块侧状态只显示），温度阈值也未定。
- cJSON 静态编入产物：板上**不需要**装 `libcjson`/`libcjson-dev`，也不需要重建
  armhf 工具链镜像；正常 `./build.sh gui-armhf` 后把 `build-armhf/deffire-gui-dev`
   拷到板上即可（`Dockerfile.build-armhf` 已经 `COPY third_party/`）。

### 2.6 串口来源（2026-11）

**传输方式定为字节流**：每路信息源一个独立串口设备（不是 GPIO 开关量）。
实现：`include/serial_source.h` + `src/library/serial_source.cpp`。

- 一路一条记录：设备路径 + 波特率。设备清单**运行时枚举**
  （`serial_list_devices()` 扫 `/dev/tty*`，排除 `/dev/tty`、`/dev/ttyprintk`、
  虚拟控制台 `/dev/tty0..N`），波特率是标准值清单
  （9600/19200/38400/57600/115200/230400/460800/921600）。**不预置任何默认值。**
- 打开：`O_RDWR | O_NOCTTY | O_NONBLOCK` + `cfmakeraw` + 8N1 + `CLOCAL|CREAD`。
- **一个后台线程** `poll()` 所有已打开端口，读到字节就
  `json_source_feed(index, buf, n)` —— 不开 12 个线程（单核 A7）。
- 断线（EOF / 读错误）：关 fd、状态记一条错误、`json_source_reset(index)`，
  **每秒**重连；重连成功同样 reset（旧流半帧作废）。控制台/界面状态都跟着变。
- 无界面配置：`FIRECONTROL_SERIAL_<N>="<device>:<baud>"`（N = 1..12）；
  格式不对只报一行、不崩。
- 退出：`cpp_start()` 在 `gui_teardown()` **之前**调 `serial_stop()`（关端口 + join 线程）。
- 界面：右栏「串口设备 / 波特率 / 【刷新串口】/【应用设置】」；原来的"GPIO 接口 / 线号"
  已去掉（见 §2.3）。两个下拉都选了才打开；有一个没选 = 取消配置并关端口。
- 测试：`tests/test_serial_source.cpp` 用伪终端 (`posix_openpt`) 冒充串口，
  覆盖打开/关闭、完整帧、半帧、断线检测、非法波特率、环境变量格式错误；
  原生 `ctest` 4/4，交叉编译时跳过。
- **板上待验证**：真串口路径/波特率、`dialout` 权限、拔线重连、长时间无帧。

---

## 3. 界面的约定（改 `src/appgui/gui.cpp` 之前先读）

界面是**单屏 + 分页底栏**：顶栏（品牌 / 状态灯 / 设置 / 退出）所有页完全一致，切页只重建底栏按钮。

1. **`Action::Nothing` 必须留在枚举第一位。** `ButtonSpec` 是聚合类型，表里漏写 `action`
   字段时它会被值初始化为 `0`；第一位是 `Nothing` 就退化成"占位"（点了没反应），
   第一位是别的（比如 `LedOn`）就会**静默点亮 LED**。
2. **`onAction()` 的 `switch` 不写 `default`。** 往 `Action` 加成员忘了加 `case` 时，
   `-Wswitch` 会在编译期报出来。别为了消警告去加 `default`（要消 `-Wformat-overflow`
   就给结果变量一个非空初值）。
3. **加页面 / 加按钮只改表**：`kHomeButtons`（12 个信息源）/ `kSettingsButtons`（本地测试）/
   `kSensorButtons`（覆盖层背后不可见的占位底栏）/ `kLedButtons` / `kBuzzerButtons` / `kOutBuzzerButtons` + `kPages`。
   数组长度用 `countOf()` 取，**不要手写数字** —— 手写的 `kNumButtons` 和表对不上，
   已经坏过一次（越界初始化）。
   **"每行放几个"是每页自己的参数**（`PageSpec::columns`，首页 6 列、设置/调试页 4 列、
   信息源页 1 列），不再是全局常量；行数由 `rowsOf(count, columns)` 算。末行不满时
   `buildPageButtons()` 会补**透明占位**，让各行按钮等宽（不补的话 `flex_grow` 会把
   末行少量按钮撑成半屏宽）。`columns` 必须 ≥ 1（有 `static_assert` 挡住除零）。
   只有一个按钮的页（`Page::Sensor` 背后不可见的占位底栏）走 `single` 分支：行居中、
   按钮固定 25% 宽；接口页真正的返回入口是覆盖层顶栏的【首页】。
4. **`Page` 枚举的顺序必须与 `kPages` 一致**（有 `static_assert` 兜着）。
   `Page::Sensor` 是**一类页**，具体编号在 `g_sensor_index`；页名由 `pageName()` 动态生成。
5. **切页只置 `g_page_dirty`，重建在主循环里做。** 不要在事件回调里删掉正在派发的那个
   按钮所在的底栏；信息源页的数据面板也在同一个 dirty 分支里显隐。
6. **控制台那行 `点击【x】坐标 (x,y) → 结果` 的格式不要改。** 板上核对触摸命中、
   记录硬件响应全靠它（runbook §4.2 的验收表按这个格式写的）。
7. `kText = "halloworld"` 是**有意保留**的（WARNING A-3）。内容区那行小字显示"页名 ·
   上一步结果"，也别删 —— 板上不接控制台时它是唯一的操作反馈。下面那行
   `JSON  温度=...  火焰=...  ...` 是 `json_status` 的解析结果（只列有的字段），
   同样保留（失败时显示"未识别"）。
8. **信息源页是独立全屏覆盖层**：`g_sensor_page` 只在 `Page::Sensor` 显示，盖住普通页的
   顶栏/底栏。布局是「顶栏(品牌/首页/退出) + 左侧 12 个信息源按钮 + 中间接收/模块状态
   卡片 + 右侧信息源设置」。左侧切换模块；右侧是**串口设备 + 波特率** 下拉 +
   【刷新串口】+【应用设置】（原来的 GPIO 接口/线号 已去掉，见 §2.3）；
   中间五行是烟雾/火焰/温度/蜂鸣器/LED，下面是模块状态、收到的完整报文、预留。
   0=正常/1=异常由传感器模块定义，app 不做极性换算；五行来自 payload 的
   `smoke_state` / `fire_state` / `temperature` / `buzzer_state` / `led_state`，
   缺字段显示占位；蜂鸣器/LED 是模块侧状态机，app 只显示。**没有 12 路默认串口配置**——现场用 `FIRECONTROL_SERIAL_<N>` 或界面选。
   覆盖层使用 `kSensorBg` 等深蓝黑 + 细灰线 + 少量琥珀强调，不套用普通页的深色工业风。
9. `q` / `Esc` 在每一页都是退出程序（板子是触摸屏，键盘只给开发机用）。

---

## 4. 协作上的硬约束

- **远程用 SSH，不要用 HTTPS。** `git@github.com:Skywindfox/Skywind_Fox_Firefighting_Main.git` ——
  `github.com` 的 HTTPS 时通时断，推一半失败很难查。
- **开发机跑不了 armhf 产物**（架构不同 + 需要板上的 libX11），必须 scp 到板上跑。
- **板上不编译**：一切从开发机交叉编译后推过去，产物统一放板上 `~/Desktop`。
- **测试以手工验收为主**：硬件 / GUI / 触摸走 [VERIFY-RUNBOOK.md](VERIFY-RUNBOOK.md) 的逐项判据；
  `ctest` 只覆盖纯逻辑层（当前 `greeting` / `json_status` / `json_stream`）。不追求把硬件行为 mock 成自动化测试 ——
  那种测试证明不了板上的事。
- **`~/下载/` 里的正点原子资料别照抄**：`main.c` 是"综合例程扩展板"的配置（3 个 LED、
  蜂鸣器走 `EV_SND`），**不是手上这块底板**（1 个 LED + 1 个蜂鸣器，都在
  `/sys/class/leds/` 下）；那份 PDF 讲的是出厂 Buildroot 系统，指令不能直接搬到 Debian。
  IIO 的 `iio:deviceN` 编号是动态分配的，**别写死**。

### 4.1 PR 处理记录（2026-09-19）

| PR | 状态 | 说明 |
|---|---|---|
| #7 | ✅ 已合并 | 权限脚本覆盖 `/dev/gpiochip*`（建 `gpio` 组 + 第二条 udev 规则）；板上 `gpiodetect` 已不再 Permission denied |
| #8 | ✅ 已合并 | 外部无源蜂鸣器入口 + 独立调试页（merge `c725907`）。合并后我在 main 上补了两个提交：`67a83a9` 修实现（line 只 request 一次 + 真方波）、`2966915` 把发声移到**专用线程**（默认 2 kHz、`FIRECONTROL_BUZZER_HZ` 可调） |
| #9 | ❌ 决定关闭 | 想把发声回退成"每轮一个脉冲 + `delay_us(500)`"：与 main **全面冲突**（基于 `67a83a9`），而且它针对的"音调低"已由 `2966915` 解决；回退会带回 1 kHz 上限、每轮阻塞 1 ms、每轮 2 次芯片开关。**待有人在网页上点 Close** |

**工具限制**：本机没装 `gh`，也没有 GitHub token —— 仓库是公开的，所以**读**（列 PR/issue、
看 diff、读正文）走 GitHub API 免认证即可；但**评论、关闭、点 Merge 按钮**都要在网页上做。
合并可以走 `git fetch origin pull/N/head:pr-N` + 本地 merge + `git push`：GitHub 会认作
已合并（#7/#8 都是这么合的，`merged=True`）。

**流程教训（犯过一次）**：用户说"整合 PR"时，我先去改代码、**没有重新拉一次开放 PR 列表**
—— 结果漏掉了在改代码期间新开的 #9。**以后再动手前先 `git fetch --all --prune` 并把开放
PR/issue 列一遍**，动手前和推送前各查一次。
