#ifndef FIRECONTROL_JSON_STATUS_H
#define FIRECONTROL_JSON_STATUS_H

#include <cstddef>
#include <string>

namespace firecontrol
{
    // 信息源数量: 12 个模块, 每个模块一路独立输入。
    inline constexpr int kInfoSourceCount = 12;

    // 最基础的 JSON 状态 —— 目前只认两个数字字段:
    //
    //     {"data":1,"smoke":1}
    //
    // 解析失败时 valid=false, error 指向一条静态说明; raw 保留最近一次 payload。
    struct JsonStatus {
        bool valid = false;
        bool has_data = false;
        bool has_smoke = false;
        int data = 0;
        int smoke = 0;
        const char *error = nullptr;  // 静态字符串; nullptr = 没有错误
        unsigned revision = 0;        // 值/完整帧变化时递增
        char raw[256] = {};           // 最近一次 payload 原文 (可能被截断)

        // 最近一次"完整帧", 包含 {"start":1} 和 {"end":1}。
        // has_frame=false 表示这一路还没收到过完整帧。
        bool has_frame = false;
        std::string frame;
    };

    // 解析一段 JSON 文本。成功返回 0, 失败 -1 (out->error 说明原因)。
    int json_parse(const char *text, JsonStatus *out);

    // 从文件读取并解析 (不经过 start/end 切帧, 给测试/旧格式用)。
    int json_load_file(const char *path, JsonStatus *out);

    // 初始化所有信息源。
    // 信息源 0 的来源优先级:
    //   1. FIRECONTROL_JSON       —— 环境变量里的 JSON 字面量
    //   2. FIRECONTROL_JSON_FILE  —— 环境变量指定的文件
    //   3. ./firecontrol.json     —— 默认文件 (存在才用)
    //   4. 内置示例帧
    // 信息源 N (N=2..12) 只读取对应的 FIRECONTROL_JSON_FILE_<N>;
    // 没设置时保持空状态, 等独立的串口/文件输入。
    int json_status_init();

    // 刷新所有文件来源。文件内容按追加/重写两种情况喂给各自的帧解析器。
    int json_status_refresh();

    // 串口读取线程收到字节后调用: 把这一段喂给指定信息源的帧解析器。
    // 返回本次切出的完整帧数量。线程安全。
    int json_source_feed(int source_index, const char *data, std::size_t len);

    // 串口重连/重新开始时清掉这一路的半帧和状态 (已发布的 UI 会显示空)。
    void json_source_reset(int source_index);

    // 取某一信息源的最近状态 (拷贝, 线程安全)。
    JsonStatus json_status_for(int source_index);

    // 信息源 0 的便捷入口 (普通页的 JSON 状态行用)。
    JsonStatus json_status();
}

#endif // FIRECONTROL_JSON_STATUS_H
