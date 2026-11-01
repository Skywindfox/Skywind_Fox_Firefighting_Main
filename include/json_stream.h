#ifndef FIRECONTROL_JSON_STREAM_H
#define FIRECONTROL_JSON_STREAM_H

#include <cstddef>
#include <deque>
#include <string>

namespace firecontrol
{
    // 从字节流里抠出**完整的 JSON 对象**。
    //
    // 传感器模块自己就带完整的报文头/尾 (可能是 {"start":1}/{"end":1}, 也可能是
    // 别的字节), 所以 app 侧**不再要求任何特定标记**: 只认 `{ ... }`, 花括号平衡
    // 就算一条完整对象; 对象之外的字节 (模块的头/尾、换行、垃圾) 一律丢弃。
    //
    // 这样无论模块发的是 `{"a":1}` 还是 `AA 55 {"a":1} 0D 0A`, 都能切出来。
    // 字符串里的 { } 和转义 \" 不会被误当成括号。
    class JsonObjectSplitter
    {
    public:
        // 清空缓冲和已切出的对象。
        void reset();

        // 追加收到的字节并尝试切出完整对象。可以分块喂, 对象被拆到两个 chunk
        // 里也能接上。
        void feed(const char *data, std::size_t len);
        void feed(const std::string &text);

        // 取出一条完整对象 (只含 `{...}` 本身)。没有时返回 false。
        bool pop(std::string &object);

        // 当前是否已经看到 '{'、正在等右括号 (调试/测试用)。
        bool collecting() const;

    private:
        void process();

        std::string buffer_;
        std::deque<std::string> objects_;
        bool collecting_ = false;
    };

    // 单条对象的上限: 超过就丢掉当前这条, 从下一个 '{' 重新开始 —— 防止一直等不到
    // 右括号时缓冲区无限增长。
    inline constexpr std::size_t kJsonObjectMaxBytes = 8192;
}

#endif // FIRECONTROL_JSON_STREAM_H
