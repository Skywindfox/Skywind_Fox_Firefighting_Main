#include "start.h"

#include <atomic>    // 蜂鸣器开关标志 (跨线程)
#include <cstdint>   // uint64_t
#include <cstdio>
#include <cstdlib>   // getenv / strtol (蜂鸣器频率)
#include <string>
#include <pthread.h> // 蜂鸣器发声线程
#include <time.h>    // clock_gettime / clock_nanosleep

#include "greeting.h"
#include "hardware_path.h"
#include "json_status.h"
#include "serial_source.h"
#include "usetools.h"

// ---------------------------------------------------------------------------
// 项目的主要内容都在本文件。
//
// main.cpp 只做两件事: 把命令行参数放进全局变量, 然后调用 cpp_start()。
//
// **主循环只有一处** —— 就在下面的 cpp_start() 里:
//
//     device_init();                                  // 硬件/配置, 一次
//     gui_build();                                    // 建界面, 一次
//     while (!g_quit) { status(); render(); }         // 每轮: 模块 + 一帧界面
//     gui_teardown();                                 // 收尾
//
// 这样"循环在哪"、"UI 在哪建"、"模块在哪采"各只有一处:
//   status()     —— 各模块的采集都挂这里(传感器/状态…), 内部自带节流
//   render()     —— 刷一帧界面(gui.cpp); 控制台版第一帧就置 g_quit
//   gui_build()  —— 建界面(gui.cpp); 控制台版是空实现(gui_stub.cpp)
//
// 各阶段的声明在 start.h, 所以这里不需要再写前置声明。
// ---------------------------------------------------------------------------

namespace {

// 单调时钟微秒。status() 的节流和蜂鸣器线程的节拍都用它。
uint64_t monotonic_us()
{
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ull +
           static_cast<uint64_t>(ts.tv_nsec) / 1000ull;
}

}  // namespace

int cpp_start()
{
    if (device_init() < 0) {
        std::printf("初始化失败, 退出\n");
        return 1;
    }

    // 建界面只做一次。失败的具体原因 gui_build() 自己已经打到 stderr 了
    // (例如"无法连接 X server"), 这里不重复刷屏。
    if (gui_build() < 0) {
        return 1;
    }

    // ★ 主循环 —— 整个程序只有这一处
    while (!g_quit) {
        status();  // 各模块(自带节流, 见 status())
        render();  // 刷一帧界面
    }

    // 串口读取线程要在拆界面之前停: 它会往 json_status 里写, 不该在退出过程中
    // 还在读设备。
    firecontrol::serial_stop();

    gui_teardown();
    return 0;
}

// ---------------------------------------------------------------------------
// 初始化: 开设备、读配置、建数据。
// 返回值: 0 成功, 负数表示失败(由 cpp_start 决定要不要继续)。
// ---------------------------------------------------------------------------
int device_init()
{
    std::printf("%s\n", firecontrol::greeting().c_str());

    // JSON 状态: 解析传感器模块的 {"temperature":..,"fire_alarm":..,...};
    // 旧的 {"data":1,"smoke":1} 仍然认。
    // 来源优先级见 json_status.h (环境变量字面量 > 文件 > 内置示例)。
    // 解析失败不阻断启动: status() 会把错误当成一种状态显示出来。
    firecontrol::json_status_init();

    // 串口来源: 每路一个独立设备。设备/波特率由界面选, 或用环境变量先配好:
    //     FIRECONTROL_SERIAL_2="/dev/ttyS0:115200"
    // 哪一路都没配也没关系 —— 线程空转, 什么都不做。
    firecontrol::serial_apply_env();
    firecontrol::serial_start();

    return 0;
}

// ---------------------------------------------------------------------------
// 板载 GPIO 开关量输入 (传感器)
//
// 和本文件里 LED / 蜂鸣器的区别: 那些是**输出**(往 sysfs 写), 这里是**输入**
// (读 GPIO 电平), 走 libgpiod —— 见 useable_tools::gpio_read_value() 与
// workflow.md §2.3。
//
// 接法当"数据"处理: 全写在下面这张表里, 换传感器 / 换引脚只改表, 逻辑不动。
//
// ⚠ **这张表现在是空的, 而且是故意的。** 这块板子上到底接了哪些开关量输入、
//   接在哪个 GPIO 控制器的哪条线上, 还没确认。**不预置任何"看起来合理"的传感器
//   名** —— 一个猜出来的名字会让人以为那就是需求, 比空着糟得多。
//
//   怎么填 (板上实测, 见 workflow.md §2.3 — 那里有 2026-09-19 的线上表):
//       sudo apt install gpiod
//       gpiodetect      # 有哪些控制器; **方括号里那个才是 chip_label** (如 GPIOA)
//       gpioinfo        # 每条线的编号; consumer 列是"谁占着", unused 才是空闲
//   一行填一个, 字段写全, 例如:
//       {"烟感", "GPIOA", 12, "firecontrol-smoke"},   // 填 label, 不是 "gpiochip0"
//   填完在板上跑控制台程序验证 (它会走 cpp_start() → status()):
//       cd ~/Desktop && ./deffire-dev
//
// ⚠ 填了名字但 chip_label 还是 nullptr 的行会被明确跳过并报"引脚未实测", 不会拿
//   一个猜出来的引脚去读 —— 那种"读回来是 0"最容易被当成"没有火警"。
//
// ⚠ 电平极性(高电平代表"检测到"还是"没检测到")同样要实测确认, 所以这里只返回
//   原始电平 0/1, **不做** "1 == 报警" 这种假设。极性搞反会让"有人"显示成"没人",
//   比读不到更危险。
//
// ⚠ 板载 LED / 蜂鸣器的线已经归内核 leds-gpio 驱动持有 —— 板上实测:
//   sys-led = GPIOI:3、beep = GPIOF:8, `gpioinfo` 里 consumer 就是这两个名字、
//   状态 [used]。所以这里再 request 会失败(EBUSY), 它们继续走 sysfs, 别挪进这张表。
// ---------------------------------------------------------------------------
namespace {

struct GpioInput {
    const char *name;        // 传感器名, 打印用
    const char *chip_label;  // GPIO 控制器的 label (gpiodetect 查); nullptr = 还没实测
    unsigned int line;       // 该控制器内的线号 (gpioinfo 查)
    const char *consumer;    // 传给内核的消费者名, gpioinfo 的 "used by" 会显示它
};

// 接线表: **填一行读一行**; name == nullptr 的行(占位 / 没填)自动跳过。
// 现在只留一个占位行 —— 不预置猜出来的传感器, 理由见上面的说明。
constexpr GpioInput kGpioInputs[] = {
    {},  // 占位: name == nullptr, 会被跳过
};

constexpr int kGpioInputSlots =
    static_cast<int>(sizeof(kGpioInputs) / sizeof(kGpioInputs[0]));

// 表里已经填了几路 (name != nullptr)
constexpr int countFilledInputs()
{
    int n = 0;
    for (const GpioInput &in : kGpioInputs) {
        if (in.name != nullptr) {
            n++;
        }
    }
    return n;
}

// 读一路传感器。
// 返回值: 0 成功(电平写进 out_level); -1 失败(引脚未实测 / chip 打不开 / 线被占用)。
// 失败原因直接打到 stderr —— 读不到就是读不到, 不返回一个"看起来正常"的 0。
int gpio_input_read(const GpioInput &in, int *out_level)
{
    if (out_level == nullptr) {
        return -1;
    }

    if (in.chip_label == nullptr) {
        std::fprintf(stderr,
                     "传感器【%s】的引脚还没实测: 先在板上跑 gpiodetect / gpioinfo, "
                     "把 chip_label 和 line 填进 start.cpp 的 kGpioInputs\n",
                     in.name);
        return -1;
    }

    const int level = useable_tools::gpio_read_value(in.chip_label, in.line, in.consumer);
    if (level < 0) {
        // 具体原因(chip 打不开 / 线被占用 / 没编入 libgpiod)由 gpio_read_value
        // 内部逐条报告; 这里补一句"是哪一路", 免得日志里分不清。
        std::fprintf(stderr, "传感器【%s】读取失败 (%s:%u)\n", in.name, in.chip_label,
                     in.line);
        return -1;
    }

    *out_level = level;
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// 状态采集: **各模块都挂这里** —— 主循环每轮都会调它, 所以硬件采集在内部节流。
//
// 为什么节流: 主循环大约 1 ms 一轮(GUI 的定时器节奏), 而读一遍传感器/模块
// 不需要这个频率 —— 200 ms(5 Hz)对状态量完全够, 板上单核也少烧 CPU。
// 第一次调用总是立刻跑, 之后按 kStatusPeriodUs 限频。
//
// 打印同样按"变化"触发: 采样每次都做, 只有状态变了才打一行 —— 否则 5 Hz 的
// 日志会把终端刷满, 而没变化的信息本来也没有价值。
//
// 返回值: 0 —— 单路失败不算整体失败(原因逐条打在 stderr); 负数留给将来
// "整个采集流程都起不来"那种情况。
// ---------------------------------------------------------------------------
int status()
{
    constexpr uint64_t kStatusPeriodUs = 200000;  // 200 ms
    static uint64_t last_run_us = 0;
    static int last_sig = -1;  // 上次打印时的状态签名; -1 = 还从没打印过

    const uint64_t now = monotonic_us();
    if (last_run_us != 0 && now - last_run_us < kStatusPeriodUs) {
        return 0;  // 还没到下一次采集
    }
    last_run_us = now;

    // JSON 状态: 来源是文件时重新读一次; 字面量/内置示例不会变。
    // 失败只更新状态, 不让整个采集流程失败。
    firecontrol::json_status_refresh();

    // 每个信息源一路独立状态; 哪一路变了才打印 (revision 由 json_status.cpp 维护)。
    // 串口接上后, 各路会由 json_source_feed() 更新; 现在文件来源也走同一套。
    static unsigned last_json_revision[firecontrol::kInfoSourceCount] = {};
    for (int i = 0; i < firecontrol::kInfoSourceCount; i++) {
        const firecontrol::JsonStatus json = firecontrol::json_status_for(i);
        if (json.revision == last_json_revision[i]) {
            continue;
        }
        last_json_revision[i] = json.revision;

        // 还没有完整帧、也没有错误: 这一路还没数据, 不刷屏。
        if (!json.has_frame && json.error == nullptr) {
            continue;
        }

        if (i == 0) {
            std::printf("--- JSON 状态 ---\n");
        } else {
            std::printf("--- 信息源 %d ---\n", i + 1);
        }
        if (json.has_frame) {
            std::printf("  完整报文: %s\n", json.frame.c_str());
        }
        if (json.valid) {
            // 只打这一帧里真的有的字段 (模块可能只发其中几个)。
            std::string line = "  ";
            if (json.has_temperature) {
                char temperature[32] = {};
                std::snprintf(temperature, sizeof temperature, "temperature=%.1f  ",
                              json.temperature);
                line += temperature;
            }
            if (json.has_fire_alarm) {
                line += "fire_alarm=" + std::to_string(json.fire_alarm) + "  ";
            }
            if (json.has_smoke_alarm) {
                line += "smoke_alarm=" + std::to_string(json.smoke_alarm) + "  ";
            }
            if (json.has_buzzer_state) {
                line += "buzzer_state=" + std::to_string(json.buzzer_state) + "  ";
            }
            if (json.has_led_state) {
                line += "led_state=" + std::to_string(json.led_state) + "  ";
            }
            if (json.has_data) {
                line += "data=" + std::to_string(json.data) + "  ";
            }
            if (json.has_smoke) {
                line += "smoke=" + std::to_string(json.smoke) + "  ";
            }
            std::printf("%s\n", line.c_str());
        } else {
            std::printf("  未识别: %s\n", json.error != nullptr ? json.error : "未知错误");
        }
        std::fflush(stdout);
    }

    // 先采样, 再决定打不打印。
    const int filled = countFilledInputs();
    int levels[kGpioInputSlots] = {};
    int failed = 0;
    int sig = filled * 1000;  // 签名: 接线几路 + 每一路的电平
    for (int i = 0; i < kGpioInputSlots; i++) {
        const GpioInput &in = kGpioInputs[i];
        levels[i] = -2;  // -2 = 这一行还没填
        if (in.name == nullptr) {
            continue;
        }
        if (gpio_input_read(in, &levels[i]) != 0) {
            levels[i] = -1;
            failed++;
        }
        sig = sig * 7 + (levels[i] + 2);
    }

    // 状态没变就不打印。主循环约 1 ms 一轮、这里 5 Hz, 每轮都刷会直接把终端冲掉。
    if (sig == last_sig) {
        return 0;
    }
    last_sig = sig;

    std::printf("--- 板载开关量输入 ---\n");

    if (filled == 0) {
        // 空表不是错误, 是"还没接线 / 还没实测" —— 说清下一步做什么,
        // 不要假装读过了。
        std::printf("  还没有确认的接线: 先在板上跑 gpiodetect / gpioinfo,\n");
        std::printf("  再往 src/library/start.cpp 的 kGpioInputs 里一行填一个传感器\n");
        std::fflush(stdout);
        return 0;
    }

    for (int i = 0; i < kGpioInputSlots; i++) {
        const GpioInput &in = kGpioInputs[i];
        if (in.name == nullptr) {
            continue;  // 还没填的行
        }
        if (levels[i] >= 0) {
            std::printf("  %s (%s:%u) 电平 %d\n", in.name, in.chip_label, in.line, levels[i]);
        }
    }

    if (failed > 0) {
        std::printf("  %d/%d 路没读到 (原因见上面的 stderr)\n", failed, filled);
    }
    std::fflush(stdout);

    return 0;
}

// ---------------------------------------------------------------------------
// LED / 蜂鸣器的开与关
//
// 这两个是"动作", 不是"渲染": 点一次做一次。按钮回调里直接调它们。
//
// 正点原子的 sys-led / beep: 写 "1" 亮(响), "0" 灭(停)。
// write_File 会自动补换行并检查落盘, 所以传 "1" 就行, 不用加 "\n"。
//
// 返回值交给调用者处理: 开发机上这个路径不存在, 会返回 -1,
// 所以失败是常态, 不要在这里 printf 刷屏。
//
// 需要注意的是，这些硬件都是主板原有的硬件，所以加上_onboard_，以区分。
//
// ---------------------------------------------------------------------------
// **先写 trigger="none", 再写 brightness —— 顺序不能反**
//
// trigger 一旦生效 (比如 heartbeat), 内核就接管了 brightness, 会周期性把它改回
// 自己的值。这时手动写 brightness 立刻被覆盖, 现象是"有了心跳之后, 开/关点了
// 完全没反应", 很容易误判成硬件坏了或者权限不对。
//
// 所以每次调亮度之前先把控制权交还给 brightness。
//
// 交还这一步的返回值**故意不检查**: 不是所有设备都提供 trigger 文件, 没有它的
// 时候 brightness 本来就能直接生效, 交还失败不影响"点亮/熄灭"本身。
// 需要向调用者报告成功的只有最终那次 brightness 写入。
//
// 顺带一个效果: 因为开/关都会清 trigger, 所以"心跳停不下来"的问题也一并缓解了
// —— 点一下【LED 开】或【LED 关】就等于停掉心跳。
// ---------------------------------------------------------------------------
int led_onboard_on()
{
    useable_tools::write_File(Led_trigger_Path, "none");
    return useable_tools::write_File(Led_on_board_Path, "1");
}

int led_onboard_off()
{
    useable_tools::write_File(Led_trigger_Path, "none");
    return useable_tools::write_File(Led_on_board_Path, "0");
}

int buzzer_onboard_on()
{
    useable_tools::write_File(Buzzer_trigger_Path, "none");
    return useable_tools::write_File(Buzzer_on_board_Path, "1");
}

int buzzer_onboard_off()
{
    useable_tools::write_File(Buzzer_trigger_Path, "none");
    return useable_tools::write_File(Buzzer_on_board_Path, "0");
}

// ---------------------------------------------------------------------------
// 心跳
//
// 写的是 **trigger** 文件, 不是 brightness —— 见 hardware_path.h 里的说明。
// 往 brightness 写 "heartbeat" 内核会返回 EINVAL。
//
// 两个注意点:
//   1. 心跳期间 brightness 由内核接管, 手动写 brightness 不会生效
//      (想恢复手动控制必须先 clear_heartbeat)。
//   2. write_File 用 "w" 模式打开(截断)。sysfs 属性文件每次 write 都是独立
//      写入, 截断对它没影响; 但它对普通文件是覆盖写, 别拿它写日志。
// ---------------------------------------------------------------------------
int led_onboard_set_heartbeat()
{
    return useable_tools::write_File(Led_trigger_Path, "heartbeat");
}

int buzzer_onboard_set_heartbeat()
{
    return useable_tools::write_File(Buzzer_trigger_Path, "heartbeat");
}

int led_onboard_clear_heartbeat()
{
    // 写 "none" 而不是 "" —— 空字符串会被内核当作无效输入。
    return useable_tools::write_File(Led_trigger_Path, "none");
}

int buzzer_onboard_clear_heartbeat()
{
    return useable_tools::write_File(Buzzer_trigger_Path, "none");
}


// ---------------------------------------------------------------------------
// 外部无源蜂鸣器 (接在 GPIOA:6)
//
// **无源**蜂鸣器要的是持续方波。这一段有两个坑, 都踩过:
//
//   1. 输出线**只 request 一次** (gpio_output_open 拿手柄), 之后只
//      gpio_output_set()。早期写法是每个脉冲都 open/request/release/close ——
//      主循环每轮一次 ⇒ ~1800 次/秒的芯片开关, 而且脉冲之间线是放开的, 引脚没有
//      持续驱动, 现象就是"一连串咔哒声, 不是音调"。
//   2. **发声不能挂在主循环上**: 主循环一轮约 1 ms(LVGL 定时器的节奏), 每轮最多
//      翻一次电平 ⇒ 音调被锁死在几百 Hz, 板上实测"音调很低"。所以这里用一个
//      **专用线程**, 按 clock_nanosleep 的 绝对时刻 翻转电平 —— 频率与主循环
//      彻底无关, 主循环忙不忙都不影响音准。
//
// 频率默认 2 kHz(小无源蜂鸣器的谐振点通常在 2~4 kHz), 而且可以**在板上运行时试**:
//
//     FIRECONTROL_BUZZER_HZ=3000 DISPLAY=:0 ./deffire-gui-dev
//
// 试出最响的那个值, 再把 kOutBuzzerDefaultHz 改成它。见 workflow.md §2.4。
//
// 关着的时候线程每 5 ms 醒一次看一眼标志(几乎不耗 CPU), 线保持 request 状态、
// 输出低电平(不响)。这个线程是常驻的, 进程退出时由内核回收(line 也一并释放)。
// ---------------------------------------------------------------------------
namespace {

constexpr uint32_t kOutBuzzerDefaultHz = 2000;  // 默认音调, 可在板上试出更好的值
constexpr uint32_t kOutBuzzerMinHz = 100;       // 频率上下限: 挡住 0 和离谱的值
constexpr uint32_t kOutBuzzerMaxHz = 8000;
constexpr uint32_t kOutBuzzerIdlePollUs = 5000;  // 关着时的轮询间隔(µs)

constexpr const char *kOutBuzzerChip = "GPIOA";
constexpr unsigned int kOutBuzzerLine = 6;
constexpr const char *kOutBuzzerConsumer = "out_buzzer";

// 开关标志: GUI 的按钮回调写, 蜂鸣器线程读 —— 跨线程, 所以用 atomic
std::atomic<bool> g_out_buzzer_on {false};
std::atomic<bool> g_out_buzzer_thread_started {false};
std::atomic<bool> g_out_buzzer_quit {false};  // 让线程收尾退出(见 buzzer_out_shutdown)
pthread_t g_out_buzzer_tid {};
bool g_out_buzzer_tid_valid = false;  // 只有主线程碰这几个, 不需要 atomic

// 频率: 环境变量可覆盖, 只在起线程时解析一次
uint32_t out_buzzer_hz()
{
    const char *env = std::getenv("FIRECONTROL_BUZZER_HZ");
    if (env == nullptr || *env == '\0') {
        return kOutBuzzerDefaultHz;
    }
    const long v = std::strtol(env, nullptr, 10);
    if (v < static_cast<long>(kOutBuzzerMinHz) ||
        v > static_cast<long>(kOutBuzzerMaxHz)) {
        std::fprintf(stderr,
                     "FIRECONTROL_BUZZER_HZ=%s 超出范围 (%u..%u Hz), 改用默认 %u Hz\n", env,
                     kOutBuzzerMinHz, kOutBuzzerMaxHz, kOutBuzzerDefaultHz);
        return kOutBuzzerDefaultHz;
    }
    return static_cast<uint32_t>(v);
}

// 睡到 deadline, 睡醒后把 deadline 往后推一个半周期。
// 用 **绝对时刻 + TIMER_ABSTIME**: 每一拍都以"上一拍 + 半周期"为准, 不会累积漂移;
// 偶尔被抢占睡过头时, 下一拍会立刻补上, 音调不会越走越慢。
void sleep_until_next_half_period(struct timespec *deadline, uint32_t half_period_us)
{
    deadline->tv_nsec += static_cast<long>(half_period_us) * 1000L;
    while (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_nsec -= 1000000000L;
        deadline->tv_sec += 1;
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, deadline, nullptr);
}

// 蜂鸣器线程: 自己持有输出手柄(只被本线程碰), 按固定频率翻转电平。
void *buzzer_out_thread(void *)
{
    const uint32_t hz = out_buzzer_hz();
    const uint32_t half_period_us = 1000000u / (hz * 2u);  // 频率 -> 半周期(µs)

    // 先把"用的哪个频率"打出来: 即使后面 GPIO 打不开(没接/没权限), 日志里也能看到
    // 配置值 —— 板上试不同 FIRECONTROL_BUZZER_HZ 时就靠这一行对照。
    std::fprintf(stderr, "外部无源蜂鸣器: %u Hz 方波 (半周期 %u us)\n", hz, half_period_us);

    useable_tools::GpioOutput *out = nullptr;
    bool level = false;
    struct timespec deadline {};
    clock_gettime(CLOCK_MONOTONIC, &deadline);

    for (;;) {
        if (g_out_buzzer_quit.load()) {
            // 收尾: 先静音, 再把线还回去 —— 进程退出时把 PA6 留在"我们申请着"的
            // 状态不好: 别的程序(或下一次运行)想用这条线会拿到 EBUSY。
            if (out != nullptr) {
                useable_tools::gpio_output_set(out, 0);
                useable_tools::gpio_output_close(out);
                std::fprintf(stderr, "外部无源蜂鸣器: 已停止并释放 %s:%u\n", kOutBuzzerChip,
                             kOutBuzzerLine);
            }
            return nullptr;
        }

        if (!g_out_buzzer_on.load()) {
            // 停: 只在"之前还在响"时收一次低电平, 之后每轮几乎不做事
            if (out != nullptr && level) {
                useable_tools::gpio_output_set(out, 0);
                level = false;
            }
            struct timespec idle {};
            idle.tv_nsec = static_cast<long>(kOutBuzzerIdlePollUs) * 1000L;
            nanosleep(&idle, nullptr);
            clock_gettime(CLOCK_MONOTONIC, &deadline);  // 重新对齐, 免得补发一堆
            continue;
        }

        if (out == nullptr) {
            // 懒开: 第一次真要用时才碰硬件, 没接蜂鸣器的机器启动时不会报错
            out = useable_tools::gpio_output_open(kOutBuzzerChip, kOutBuzzerLine,
                                                  kOutBuzzerConsumer);
            if (out == nullptr) {
                // 打不开(没接 / 没权限 / 没编入 libgpiod): 报一次, 然后当成"停"。
                // 不是静默, 也不是每轮重试刷屏。
                std::fprintf(stderr, "外部无源蜂鸣器: 打不开 %s:%u (原因见上面的 perror)\n",
                             kOutBuzzerChip, kOutBuzzerLine);
                g_out_buzzer_on.store(false);
                continue;
            }
        }

        level = !level;
        useable_tools::gpio_output_set(out, level ? 1 : 0);
        sleep_until_next_half_period(&deadline, half_period_us);
    }
    return nullptr;
}

}  // namespace

int buzzer_out_set(bool on)
{
    g_out_buzzer_on.store(on);

    // 第一次"要响"时才起线程: 没人用蜂鸣器就不该多一个常驻线程
    if (on && !g_out_buzzer_thread_started.exchange(true)) {
        if (pthread_create(&g_out_buzzer_tid, nullptr, buzzer_out_thread, nullptr) != 0) {
            g_out_buzzer_thread_started.store(false);
            std::fprintf(stderr, "外部无源蜂鸣器: 创建线程失败\n");
            return -1;
        }
        g_out_buzzer_tid_valid = true;  // 不 detach: 退出时要 join, 见 buzzer_out_shutdown
    }
    return 0;
}

void buzzer_out_shutdown()
{
    if (!g_out_buzzer_tid_valid) {
        return;  // 一次都没响过, 没有线程要收
    }

    g_out_buzzer_on.store(false);
    g_out_buzzer_quit.store(true);
    // 线程最长 5 ms 醒一次(关着时的轮询间隔), 所以这里 join 很快返回
    pthread_join(g_out_buzzer_tid, nullptr);

    g_out_buzzer_tid_valid = false;
    g_out_buzzer_thread_started.store(false);  // 收尾之后还能再开
    g_out_buzzer_quit.store(false);
}
