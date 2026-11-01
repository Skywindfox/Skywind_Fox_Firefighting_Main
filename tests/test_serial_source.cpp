// test_serial_source.cpp — 串口来源的单元测试 (Linux pty, 不需要真硬件)
//
// 用一对伪终端 (master/slave) 冒充串口: 测试进程往 master 写字节, 后台读取线程
// 从 slave 读 → json_source_feed() 切帧 → 检查这一路的状态和字段。
//
// 覆盖: 打开/关闭、帧解析走串口这条路、半帧、断线检测、非法波特率。
// 交叉编译时 BUILD_TESTING 直接跳过 (ARM 产物在构建机上跑不了)。

#include "json_status.h"
#include "serial_source.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

namespace {

int g_failures = 0;

void check(bool cond, const std::string &what)
{
    if (cond) {
        std::printf("  [ OK ] %s\n", what.c_str());
    } else {
        std::printf("  [FAIL] %s\n", what.c_str());
        g_failures++;
    }
}

// 等一个条件成立, 最多 timeout_ms 毫秒。
template <typename Predicate>
bool waitUntil(Predicate pred, int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

bool writeAll(int fd, const std::string &text)
{
    std::size_t done = 0;
    while (done < text.size()) {
        const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n <= 0) {
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

constexpr int kSource = 1;  // 信息源 2: 不碰信息源 1 的文件/环境变量来源

} // namespace

int main()
{
    std::printf("FireControlApp 串口来源单元测试:\n");

    // 伪终端: master 由测试持有, slave 交给 serial_configure()。
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    check(master >= 0, "创建伪终端 master");
    if (master < 0) {
        std::printf("[FAIL] 无法创建伪终端\n");
        return 1;
    }
    check(::grantpt(master) == 0 && ::unlockpt(master) == 0, "解锁伪终端 slave");
    const char *slave_path = ::ptsname(master);
    check(slave_path != nullptr, "取 slave 设备路径");
    if (slave_path == nullptr) {
        ::close(master);
        return 1;
    }

    firecontrol::json_status_init();
    firecontrol::serial_start();

    // 非法波特率: 明确失败, 不静默接受
    check(firecontrol::serial_configure(0, slave_path, 12345) == -1,
          "不支持的波特率被拒绝");
    check(!firecontrol::serial_state(0).error.empty(), "拒绝时有错误说明");

    // 正常配置 + 打开
    check(firecontrol::serial_configure(kSource, slave_path, 115200) == 0,
          "配置并打开伪终端串口");
    firecontrol::SerialState state = firecontrol::serial_state(kSource);
    check(state.configured && state.opened, "状态: 已配置且已打开");
    check(state.device == slave_path && state.baud == 115200, "设备/波特率被记住");

    // 一个完整帧: 走串口读线程 → json_source_feed → 切帧 → json_parse
    const std::string frame =
        R"({"start":1}{"temperature":36.5,"fire_alarm":1,"smoker_alarm":0,"buzzer_alarm":0}{"end":1})";
    check(writeAll(master, frame), "往串口写一个完整帧");
    const bool got = waitUntil(
        [] { return firecontrol::json_status_for(kSource).has_frame; }, 3000);
    check(got, "串口线程把帧交到了这一路的状态里");

    firecontrol::JsonStatus json = firecontrol::json_status_for(kSource);
    check(json.valid, "串口帧解析成功");
    check(json.has_temperature && json.temperature == 36.5, "temperature=36.5");
    check(json.has_fire_alarm && json.fire_alarm == 1, "fire_alarm=1");
    check(json.has_smoker_alarm && json.smoker_alarm == 0, "smoker_alarm=0");
    check(json.has_buzzer_alarm && json.buzzer_alarm == 0, "buzzer_alarm=0");
    check(json.frame == frame, "完整帧原文保留");

    // 半帧: 不该发布, 补齐后应该出帧 (跨两次 read 的拼接)
    const unsigned rev_before = firecontrol::json_status_for(kSource).revision;
    check(writeAll(master, R"({"start":1}{"temperature":40.0,)"), "写半帧");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    check(firecontrol::json_status_for(kSource).revision == rev_before,
          "半帧没有发布");
    check(writeAll(master, R"("fire_alarm":0}{"end":1})"), "补齐半帧");
    const bool merged = waitUntil(
        [] {
            return firecontrol::json_status_for(kSource).has_temperature &&
                   firecontrol::json_status_for(kSource).temperature == 40.0;
        },
        3000);
    check(merged, "补齐后新帧被解析 (temperature=40.0)");

    // 断线: 关掉 master, 读取线程应发现 EOF/错误并关掉端口、进重连等待
    ::close(master);
    const bool disconnected = waitUntil(
        [] { return !firecontrol::serial_state(kSource).opened; }, 3000);
    check(disconnected, "断线被发现 (端口已关闭)");
    check(!firecontrol::serial_state(kSource).error.empty(), "断线有错误说明");

    // 取消配置
    check(firecontrol::serial_configure(kSource, nullptr, 0) == 0, "取消配置");
    check(!firecontrol::serial_state(kSource).configured, "状态: 未配置");

    firecontrol::serial_stop();

    // 环境变量: 格式不对时只报一句、不崩; 没配上的不算成功。
    ::setenv("FIRECONTROL_SERIAL_4", "no-colon-here", 1);
    check(firecontrol::serial_apply_env() == 0, "FIRECONTROL_SERIAL 格式不对时不配置");
    check(!firecontrol::serial_state(3).configured, "状态仍是未配置");
    ::unsetenv("FIRECONTROL_SERIAL_4");

    if (g_failures != 0) {
        std::printf("[FAIL] 共 %d 项失败\n", g_failures);
        return 1;
    }
    std::printf("[ OK ] 全部通过\n");
    return 0;
}
