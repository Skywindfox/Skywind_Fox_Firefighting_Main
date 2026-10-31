// test_json_frame.cpp — JSON 帧解析的单元测试
//
// 帧格式: {"start":1} ...数据... {"end":1}
// 重点验证: 分块到达、标记被拆开、不完整帧被丢弃、下一个 start 重新开始。

#include "json_frame.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool cond, const std::string &what)
{
    if (cond) {
        std::cout << "  [ OK ] " << what << "\n";
    } else {
        std::cout << "  [FAIL] " << what << "\n";
        g_failures++;
    }
}

std::vector<std::string> feed(firecontrol::JsonFrameParser &parser, const std::string &text)
{
    parser.feed(text);
    std::vector<std::string> frames;
    std::string frame;
    while (parser.pop(frame)) {
        frames.push_back(frame);
    }
    return frames;
}

const char *kStart = R"({"start":1})";
const char *kEnd = R"({"end":1})";

} // namespace

int main()
{
    std::cout << "FireControlApp JSON 帧单元测试:\n";

    {
        firecontrol::JsonFrameParser p;
        auto frames = feed(p, std::string(kStart) + R"({"data":1,"smoke":1})" + kEnd);
        check(frames.size() == 1, "一个完整帧被解析出来");
        check(frames.size() == 1 && frames[0] ==
                  std::string(kStart) + R"({"data":1,"smoke":1})" + kEnd,
              "完整帧包含 start/数据/end");
        check(!p.collecting(), "完整帧之后不在收集状态");
    }

    {
        firecontrol::JsonFrameParser p;
        check(feed(p, std::string(kStart) + R"({"data":1)").empty(), "只有 start+部分数据时不出帧");
        check(p.collecting(), "正在等 end");
        auto frames = feed(p, std::string(R"(,"smoke":1})") + kEnd);
        check(frames.size() == 1, "补齐 end 后出帧");
    }

    {
        firecontrol::JsonFrameParser p;
        check(feed(p, R"({"star)").empty(), "部分 start 标记不出帧");
        check(!p.collecting(), "部分 start 还不算进入帧");
        check(feed(p, R"(t":1}{"data":1})").empty(), "start 补全后仍等 end");
        check(p.collecting(), "start 补全后进入收集状态");
        auto frames = feed(p, kEnd);
        check(frames.size() == 1, "end 到达后出帧");
    }

    {
        firecontrol::JsonFrameParser p;
        check(feed(p, std::string(kStart) + R"({"data":1}{"en)").empty(), "end 只到一半时不出帧");
        auto frames = feed(p, "d\":1}");
        check(frames.size() == 1, "end 补全后出帧");
    }

    {
        firecontrol::JsonFrameParser p;
        const std::string input =
            "garbage" + std::string(kStart) + R"({"data":1})" + kEnd +
            "middle" + std::string(kStart) + R"({"data":2})" + kEnd + "tail";
        auto frames = feed(p, input);
        check(frames.size() == 2, "一次收到两个完整帧");
        check(frames.size() == 2 &&
                  frames[0].find(R"("data":1)") != std::string::npos &&
                  frames[1].find(R"("data":2)") != std::string::npos,
              "两个帧的内容和顺序正确");
    }

    {
        firecontrol::JsonFrameParser p;
        // 第一个帧没结束, 后面又出现一个 start: 应丢弃第一个, 从新 start 开始。
        const std::string input =
            std::string(kStart) + R"({"data":1})" +
            std::string(kStart) + R"({"data":2})" + kEnd;
        auto frames = feed(p, input);
        check(frames.size() == 1, "不完整帧后面跟新 start: 只出一个完整帧");
        check(frames.size() == 1 && frames[0].find(R"("data":1)") == std::string::npos,
              "旧的不完整帧被丢弃");
        check(frames.size() == 1 && frames[0].find(R"("data":2)") != std::string::npos,
              "从新的 start 重新收集");
    }

    {
        firecontrol::JsonFrameParser p;
        check(feed(p, std::string(kStart) + R"({"data":1})").empty(), "缺 end 的帧不出");
        check(p.collecting(), "缺 end 时仍在收集");
        auto frames = feed(p, std::string(kEnd) + " 后面的垃圾");
        check(frames.size() == 1, "补上 end 后出帧");
        check(frames.size() == 1 && frames[0] ==
                  std::string(kStart) + R"({"data":1})" + kEnd,
              "帧内容正确, 垃圾被忽略");
    }

    {
        firecontrol::JsonFrameParser p;
        check(feed(p, "no frame here").empty(), "没有 start 时不出帧");
        check(!p.collecting(), "没有 start 时不在收集状态");
    }

    {
        firecontrol::JsonFrameParser p;
        feed(p, std::string(kStart) + R"({"data":1})");
        p.reset();
        check(!p.collecting(), "reset 清掉未完成的帧");
        auto frames = feed(p, std::string(kStart) + R"({"data":2})" + kEnd);
        check(frames.size() == 1 && frames[0].find(R"("data":2)") != std::string::npos,
              "reset 后可以从新帧开始");
    }

    if (g_failures != 0) {
        std::cout << "[FAIL] 共 " << g_failures << " 项失败\n";
        return 1;
    }

    std::cout << "[ OK ] 全部通过\n";
    return 0;
}
