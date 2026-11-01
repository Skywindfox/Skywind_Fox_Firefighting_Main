#ifndef FIRECONTROL_JSON_STATUS_H
#define FIRECONTROL_JSON_STATUS_H

#include <cstddef>
#include <string>

namespace firecontrol
{
    // 信息源数量: 12 个模块, 每个模块一路独立输入。
    inline constexpr int kInfoSourceCount = 12;

    // 传感器 (探测版) 上报的报文。模块实际发的是带信封的一整条 JSON:
    //
    //   {"header":"SENSOR_DATA","data":{"temperature":20.06,"int_dev":1,
    //    "int_err":0,"fire_alarm":1,"smoke_alarm":0,"led_state":0,
    //    "buzzer_state":0},"footer":"END"}\r\n
    //
    // - 状态字段在 **data 子对象**里; 有 data 对象就只从里面取, 没有则按旧扁平
    //   格式从根对象取 (兼容早期报文/测试)。
    // - header / footer / int_dev / int_err 一律忽略 (信封和调试字段)。
    //
    // 字段含义 (模块侧定义, app 只显示, 不做判断):
    //   fire_alarm    0=无火 1=有火
    //   smoke_alarm   0=无烟 1=有烟
    //   buzzer_state  0=灭 1=急促(100ms 响/停) 2=缓慢(500ms) 3=长响
    //   led_state     0=灭 1=蓝闪(仅烟) 2=绿闪(仅温) 3=红闪(仅火)
    //                 4=蓝↔绿(烟+温) 5=蓝↔红(烟+火) 6=绿↔红(温+火)
    //                 7=红→绿→蓝(三路全报)
    //   temperature   温度值 (单位由模块定, app 原样显示)
    //
    // 旧字段名 fire_state / smoke_state / smoker_alarm / buzzer_alarm 作为**别名**
    // 也认; 旧的 {"data":1,"smoke":1} 同样保留兼容。
    //
    // 字段都是**可选**的: 缺哪个 has_* 就是 false, 不算解析失败 —— 模块可能只发
    // 它有的那几个。至少认出 1 个字段才算成功; 一个都没有则 error="没有已知字段"。
    // 字段存在但类型不对(不是数字)才算失败。
    //
    // 解析失败时 valid=false, error 指向一条静态说明; raw 保留最近一次 payload。
    struct JsonStatus {
        bool valid = false;

        // 旧格式 {"data":1,"smoke":1} (保留兼容)
        bool has_data = false;
        bool has_smoke = false;
        int data = 0;
        int smoke = 0;

        // 模块状态机字段 (变量名同固件, 见上面的说明)
        bool has_temperature = false;
        double temperature = 0.0;  // 单位由模块定, app 原样显示
        bool has_fire_alarm = false;
        int fire_alarm = 0;
        bool has_smoke_alarm = false;
        int smoke_alarm = 0;
        bool has_buzzer_state = false;
        int buzzer_state = 0;
        bool has_led_state = false;
        int led_state = 0;

        const char *error = nullptr;  // 静态字符串; nullptr = 没有错误
        unsigned revision = 0;        // 值/完整对象变化时递增
        char raw[256] = {};           // 最近一次 payload 原文 (可能被截断)

        // 最近一次收到的**完整 JSON 对象**原文 (不含模块自己的报文头/尾)。
        // has_frame=false 表示这一路还没收到过完整对象。
        bool has_frame = false;
        std::string frame;
    };

    // 解析一段 JSON 文本。成功(认出至少一个已知字段)返回 0, 失败 -1
    // (out->error 说明原因: 语法错误 / 根不是对象 / 字段类型不对 / 没有已知字段)。
    int json_parse(const char *text, JsonStatus *out);

    // 从文件读取并解析 (按整段 JSON 解析, 给测试/旧格式用)。
    int json_load_file(const char *path, JsonStatus *out);

    // 初始化所有信息源。
    // 信息源 0 的来源优先级:
    //   1. FIRECONTROL_JSON       —— 环境变量里的 JSON 字面量
    //   2. FIRECONTROL_JSON_FILE  —— 环境变量指定的文件
    //   3. ./firecontrol.json     —— 默认文件 (存在才用)
    //   4. 内置示例对象
    // 信息源 N (N=2..12) 只读取对应的 FIRECONTROL_JSON_FILE_<N>;
    // 没设置时保持空状态, 等独立的串口/文件输入。
    int json_status_init();

    // 刷新所有文件来源。文件内容按追加/重写两种情况喂给各自的字节流切分器。
    int json_status_refresh();

    // 串口/总线读取线程收到字节后调用: 把这一段喂给指定信息源的字节流。
    // app 侧只从字节流里抠 `{...}` 完整 JSON 对象 (模块自己的报文头/尾会被跳过);
    // 返回本次切出并发布的完整对象数量。线程安全。
    int json_source_feed(int source_index, const char *data, std::size_t len);

    // 串口重连/重新开始时清掉这一路的半个对象和状态 (已发布的 UI 会显示空)。
    void json_source_reset(int source_index);

    // 取某一信息源的最近状态 (拷贝, 线程安全)。
    JsonStatus json_status_for(int source_index);

    // 信息源 0 的便捷入口 (普通页的 JSON 状态行用)。
    JsonStatus json_status();
}

#endif // FIRECONTROL_JSON_STATUS_H
