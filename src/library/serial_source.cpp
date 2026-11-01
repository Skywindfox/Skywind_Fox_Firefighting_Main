#include "serial_source.h"

#include "json_status.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace firecontrol
{
namespace
{
constexpr int kReadBufferBytes = 1024;   // 一次 read 的上限; 帧可以跨多次 read
constexpr int kPollMs = 100;             // poll 的等待时间 (也是空闲时的睡眠)
constexpr long long kReconnectMs = 1000; // 断线后重开端口的最小间隔

// 每一路信息源一个槽位。fd < 0 表示现在没有打开; configured 表示用户已经选过
// 设备+波特率 (断线重连时还要用)。
struct Slot {
    bool configured = false;
    std::string device;
    int baud = 0;
    int fd = -1;
    std::string error;
    unsigned revision = 0;
    long long last_attempt_ms = 0;
};

Slot g_slots[kInfoSourceCount];
std::mutex g_mutex;
std::thread g_thread;
std::atomic<bool> g_running {false};

long long now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

speed_t baudSpeed(int baud)
{
    switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    default: return 0;
    }
}

// 打开并配置一个串口 (8N1、raw、非阻塞)。失败时 error 里是原因。
int openPort(const std::string &device, int baud, std::string *error)
{
    const speed_t speed = baudSpeed(baud);
    if (speed == 0) {
        if (error != nullptr) {
            *error = "不支持的波特率";
        }
        return -1;
    }

    const int fd = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        if (error != nullptr) {
            *error = std::string("打开失败: ") + std::strerror(errno);
        }
        return -1;
    }

    termios tio {};
    if (::tcgetattr(fd, &tio) != 0) {
        if (error != nullptr) {
            *error = std::string("读串口参数失败: ") + std::strerror(errno);
        }
        ::close(fd);
        return -1;
    }

    // 串口上跑的是 JSON 文本: 不要回显、不要行缓冲、不要 CR/LF 转换。
    ::cfmakeraw(&tio);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
    tio.c_cflag |= CS8;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    ::cfsetispeed(&tio, speed);
    ::cfsetospeed(&tio, speed);

    if (::tcsetattr(fd, TCSANOW, &tio) != 0) {
        if (error != nullptr) {
            *error = std::string("设置串口参数失败: ") + std::strerror(errno);
        }
        ::close(fd);
        return -1;
    }

    return fd;
}

void closePort(Slot &slot)
{
    if (slot.fd >= 0) {
        ::close(slot.fd);
        slot.fd = -1;
    }
}

// 调用者持锁。返回 0 = 打开成功。
int tryOpen(Slot &slot)
{
    slot.last_attempt_ms = now_ms();
    closePort(slot);

    std::string error;
    const int fd = openPort(slot.device, slot.baud, &error);
    if (fd < 0) {
        // 同一句错误不重复递增 revision (每秒重试一次, 不必让 UI 每秒重画)。
        if (slot.error != error) {
            slot.error = error;
            slot.revision++;
        }
        return -1;
    }

    slot.fd = fd;
    if (!slot.error.empty()) {
        slot.error.clear();
    }
    slot.revision++;
    return 0;
}

// 调用者持锁。配置有效时确保后台线程在跑。
bool ensureThreadLocked();

void runLoop();

} // namespace

std::vector<std::string> serial_list_devices()
{
    std::vector<std::string> devices;

    glob_t matches {};
    if (glob("/dev/tty*", 0, nullptr, &matches) != 0) {
        globfree(&matches);
        return devices;
    }

    for (std::size_t i = 0; i < matches.gl_pathc; i++) {
        const std::string path = matches.gl_pathv[i];
        const std::string name = path.substr(path.find_last_of('/') + 1);

        // /dev/tty 是"当前终端", /dev/ttyprintk 是内核打印, 都不是串口。
        if (name == "tty" || name == "ttyprintk") {
            continue;
        }

        // /dev/tty0.../dev/tty63 是虚拟控制台 (键盘屏幕), 也不是串口。
        if (name.size() > 3 && name.compare(0, 3, "tty") == 0) {
            const std::string suffix = name.substr(3);
            bool all_digits = true;
            for (const char c : suffix) {
                if (c < '0' || c > '9') {
                    all_digits = false;
                    break;
                }
            }
            if (all_digits) {
                continue;
            }
        }

        devices.push_back(path);
    }

    globfree(&matches);
    std::sort(devices.begin(), devices.end());
    return devices;
}

const std::vector<int> &serial_baud_rates()
{
    static const std::vector<int> kBauds = {9600,   19200,  38400,  57600,
                                            115200, 230400, 460800, 921600};
    return kBauds;
}

int serial_configure(int source_index, const char *device, int baud)
{
    if (source_index < 0 || source_index >= kInfoSourceCount) {
        return -1;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    Slot &slot = g_slots[source_index];

    const bool configured = (device != nullptr && *device != '\0' && baud > 0);
    if (!configured) {
        closePort(slot);
        slot.configured = false;
        slot.device.clear();
        slot.baud = 0;
        slot.error.clear();
        slot.revision++;
        json_source_reset(source_index);
        std::printf("信息源 %d 串口: 取消配置\n", source_index + 1);
        std::fflush(stdout);
        return 0;  // 取消不是失败
    }

    closePort(slot);
    slot.configured = true;
    slot.device = device;
    slot.baud = baud;
    slot.last_attempt_ms = 0;
    slot.revision++;

    const int rc = tryOpen(slot);
    json_source_reset(source_index);  // 新连接: 上一段的半帧不要了

    ensureThreadLocked();

    if (rc == 0) {
        std::printf("信息源 %d 串口: %s @ %d 已打开\n", source_index + 1, device, baud);
    } else {
        std::printf("信息源 %d 串口: %s @ %d 打开失败 —— %s (每秒重试)\n",
                    source_index + 1, device, baud, slot.error.c_str());
    }
    std::fflush(stdout);
    return rc;
}

SerialState serial_state(int source_index)
{
    SerialState state;
    if (source_index < 0 || source_index >= kInfoSourceCount) {
        return state;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    const Slot &slot = g_slots[source_index];
    state.configured = slot.configured;
    state.opened = slot.fd >= 0;
    state.device = slot.device;
    state.baud = slot.baud;
    state.error = slot.error;
    state.revision = slot.revision;
    return state;
}

namespace
{
bool ensureThreadLocked()
{
    if (g_running.load()) {
        return true;
    }
    g_running.store(true);
    g_thread = std::thread(runLoop);
    return true;
}

void runLoop()
{
    std::vector<pollfd> fds;
    std::vector<int> indices;

    while (g_running.load()) {
        fds.clear();
        indices.clear();

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            const long long now = now_ms();
            for (int i = 0; i < kInfoSourceCount; i++) {
                Slot &slot = g_slots[i];
                if (slot.configured && slot.fd < 0 &&
                    (slot.last_attempt_ms == 0 ||
                     now - slot.last_attempt_ms >= kReconnectMs)) {
                    if (tryOpen(slot) == 0) {
                        std::printf("信息源 %d 串口已重连: %s @ %d\n", i + 1,
                                    slot.device.c_str(), slot.baud);
                        std::fflush(stdout);
                        // 重连成功: 旧流的半帧作废。
                        json_source_reset(i);
                    }
                }
                if (slot.fd >= 0) {
                    fds.push_back(pollfd {slot.fd, POLLIN, 0});
                    indices.push_back(i);
                }
            }
        }

        if (fds.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
            continue;
        }

        const int ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), kPollMs);
        if (ready <= 0) {
            continue;
        }

        for (std::size_t k = 0; k < fds.size(); k++) {
            const short events = fds[k].revents;
            if (events == 0 || (events & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) == 0) {
                continue;
            }

            char buffer[kReadBufferBytes];
            ssize_t count = -1;
            bool failed = false;

            {
                std::lock_guard<std::mutex> lock(g_mutex);
                const int index = indices[k];
                Slot &slot = g_slots[index];

                // configure() 可能刚把这个 fd 换掉, 别读旧句柄。
                if (slot.fd != fds[k].fd) {
                    continue;
                }

                count = ::read(slot.fd, buffer, sizeof buffer);
                if (count == 0 ||
                    (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    closePort(slot);
                    const std::string error = "设备已断开（等待重连）";
                    if (slot.error != error) {
                        slot.error = error;
                        slot.revision++;
                    }
                    std::printf("信息源 %d 串口断开: %s（每秒自动重连）\n", index + 1,
                                slot.device.c_str());
                    std::fflush(stdout);
                    json_source_reset(index);
                    failed = true;
                }
            }

            if (!failed && count > 0) {
                json_source_feed(indices[k], buffer, static_cast<std::size_t>(count));
            }
        }
    }
}
} // namespace

int serial_start()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    ensureThreadLocked();
    return 0;
}

void serial_stop()
{
    if (g_running.exchange(false) && g_thread.joinable()) {
        g_thread.join();
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    for (Slot &slot : g_slots) {
        closePort(slot);
        slot.revision++;
    }
}

int serial_apply_env()
{
    int applied = 0;
    for (int i = 0; i < kInfoSourceCount; i++) {
        char name[40] = {};
        std::snprintf(name, sizeof name, "FIRECONTROL_SERIAL_%d", i + 1);
        const char *spec = std::getenv(name);
        if (spec == nullptr || *spec == '\0') {
            continue;
        }

        const std::string text(spec);
        const std::size_t colon = text.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) {
            std::fprintf(stderr, "%s=%s 格式不对, 应为 <device>:<baud>\n", name, spec);
            continue;
        }

        const std::string device = text.substr(0, colon);
        const int baud = std::atoi(text.c_str() + colon + 1);
        if (serial_configure(i, device.c_str(), baud) == 0) {
            applied++;
        } else {
            std::fprintf(stderr, "%s=%s 打开失败: %s\n", name, spec,
                         serial_state(i).error.c_str());
        }
    }
    return applied;
}
} // namespace firecontrol
