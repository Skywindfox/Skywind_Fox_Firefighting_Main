#ifndef FIRECONTROL_SERIAL_SOURCE_H
#define FIRECONTROL_SERIAL_SOURCE_H

#include <string>
#include <vector>

namespace firecontrol
{
    // 串口来源: 每路信息源一个独立串口设备 (设备路径 + 波特率), 后台一个线程
    // 用 poll() 同时盯多路, 收到字节就交给 json_source_feed(index, ...) 切帧。
    //
    // **不预置任何设备路径/波特率**: 板上有哪些设备由 serial_list_devices() 枚举,
    // 用户在哪一路用哪个由 UI 选; 无界面时可用环境变量 FIRECONTROL_SERIAL_<N>
    // (N=1..12) = "<device>:<baud>" 指定, 比如:
    //
    //     FIRECONTROL_SERIAL_2=/dev/ttyS0:115200 ./deffire-gui-dev
    //
    // 断线(EOF/读错误)不会一直卡着: 关掉 fd、状态记一条错误, 每秒尝试重开,
    // 重开成功后调用 json_source_reset(index) 清掉半帧。
    struct SerialState {
        bool configured = false;  // 用户选过设备 + 波特率
        bool opened = false;      // fd 当前可用
        std::string device;       // 空 = 未配置
        int baud = 0;             // 0 = 未配置
        std::string error;        // 最近一次打开/读失败的原因; 空 = 没有错误
        unsigned revision = 0;    // 状态变化时递增 (UI 据此决定要不要重设标签)
    };

    // 当前系统里可用的串口设备 (/dev/tty*), 已排序。用于 UI 下拉框。
    // 排除 /dev/tty 本身、/dev/ttyprintk 和虚拟控制台 (/dev/tty0...)。
    std::vector<std::string> serial_list_devices();

    // 支持的标准波特率 (UI 下拉框用它; 未知值在 serial_configure 里会失败)。
    const std::vector<int> &serial_baud_rates();

    // 给某一路配置串口并立即尝试打开。device 为空或 baud 不在支持列表 = 取消配置
    // (关闭正在用的端口)。返回 0 = 已打开; -1 = 失败 (原因见 serial_state().error)。
    // 线程安全。index 越界返回 -1。
    int serial_configure(int source_index, const char *device, int baud);

    // 取某一路的串口状态 (拷贝, 线程安全); index 越界返回空状态。
    SerialState serial_state(int source_index);

    // 启动/停止后台读取线程。serial_configure() 成功配置时会自动确保线程在跑。
    // serial_stop() 会关掉所有端口并 join 线程; 之后可以再次 serial_start()。
    int serial_start();
    void serial_stop();

    // 读环境变量 FIRECONTROL_SERIAL_<N>="<device>:<baud>" (N=1..12, 对应信息源
    // 0..11), 逐个配置。返回成功打开的路数。
    int serial_apply_env();
}

#endif // FIRECONTROL_SERIAL_SOURCE_H
