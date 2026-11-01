#ifndef FIRECONTROL_JSON_FRAME_H
#define FIRECONTROL_JSON_FRAME_H

#include <cstddef>
#include <deque>
#include <string>

namespace firecontrol
{
    // 传感器模块定义的帧格式:
    //
    //     {"start":1} ...中间是数据... {"end":1}
    //
    // 头尾归**发送方(传感器模块)**产生: app 只解析、不添加 —— 收到 start 之前的字节
    // 直接丢弃, 本解析器从不写标记, 也不会替调用方补头补尾。要换协议只改下面两个
    // 常量(或按模块实测字节改), 不要在调用方拼帧。
    //
    // 标记按**精确字节**匹配 (传感器端控制格式)。解析器是流式的: feed() 可以
    // 一次给一段, 标记被拆到两个 chunk 里也能处理。
    inline constexpr const char *kFrameStart = R"({"start":1})";
    inline constexpr const char *kFrameEnd = R"({"end":1})";

    // 单帧上限: 超过就丢弃当前帧, 从下一个 start 重新开始 —— 防止一直等不到
    // end 时缓冲区无限增长。
    inline constexpr std::size_t kFrameMaxBytes = 8192;

    class JsonFrameParser
    {
    public:
        // 清空缓冲和已完成帧。已经发布出去的状态不受影响。
        void reset();

        // 追加收到的字节并尝试切帧。
        void feed(const char *data, std::size_t len);
        void feed(const std::string &text);

        // 取出一个完整帧 (包含 start/end 标记)。没有完整帧时返回 false。
        bool pop(std::string &frame);

        // 当前是否已经看到 start、正在等 end (调试/测试用)。
        bool collecting() const;

    private:
        void process();

        std::string buffer_;
        std::deque<std::string> frames_;
        bool in_frame_ = false;
    };
}

#endif // FIRECONTROL_JSON_FRAME_H
