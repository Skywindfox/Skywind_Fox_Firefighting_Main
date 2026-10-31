// test_json_status.cpp — JSON 解析的单元测试
//
// 只测纯逻辑 (json_parse), 不碰文件/环境变量/GUI。
// 原生构建时由 ctest 调用; 交叉编译时自动跳过 (ARM 产物跑不了)。

#include "json_status.h"

#include <cstring>
#include <iostream>
#include <string>

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

bool parse(const char *text, firecontrol::JsonStatus *out)
{
    return firecontrol::json_parse(text, out) == 0;
}

} // namespace

int main()
{
    std::cout << "FireControlApp JSON 单元测试:\n";

    firecontrol::JsonStatus st;

    // 基本示例: 就是需求里那条
    check(parse(R"({"data":1,"smoke":1})", &st), "解析 {\"data\":1,\"smoke\":1}");
    check(st.valid, "valid = true");
    check(st.has_data && st.has_smoke, "data / smoke 都存在");
    check(st.data == 1 && st.smoke == 1, "data=1, smoke=1");
    check(st.error == nullptr, "成功时没有错误");

    // 空格、换行、额外字段、0/负数
    check(parse("{\n  \"data\": 0,\n  \"smoke\": -1,\n  \"extra\": \"ignored\"\n}", &st),
          "解析带空白/额外字段的对象");
    check(st.valid && st.data == 0 && st.smoke == -1, "data=0, smoke=-1, 额外字段被忽略");

    // 缺字段
    check(!parse(R"({"smoke":1})", &st), "缺少 data 时失败");
    check(!st.valid && st.error != nullptr &&
              std::string(st.error).find("data") != std::string::npos,
          "错误信息提到 data");

    check(!parse(R"({"data":1})", &st), "缺少 smoke 时失败");
    check(!st.valid && st.error != nullptr &&
              std::string(st.error).find("smoke") != std::string::npos,
          "错误信息提到 smoke");

    // 类型不对
    check(!parse(R"({"data":"1","smoke":1})", &st), "data 是字符串时失败");
    check(!st.valid, "类型错误时 valid = false");

    check(!parse(R"({"data":1,"smoke":null})", &st), "smoke 是 null 时失败");
    check(!st.valid, "null 时 valid = false");

    // 语法错误 / 根节点不是对象 / 空输入
    check(!parse("{", &st), "语法错误时失败");
    check(!st.valid, "语法错误时 valid = false");

    check(!parse("[1,2,3]", &st), "根节点是数组时失败");
    check(!st.valid, "数组时 valid = false");

    check(!parse("", &st), "空字符串失败");
    check(!st.valid, "空字符串时 valid = false");

    check(!parse(R"({"data":1,"smoke":1} trailing)", &st), "JSON 后面有垃圾时失败");
    check(!st.valid, "尾部垃圾时 valid = false");

    // 原文保留 (用于界面/排查)
    check(parse(R"({"data":2,"smoke":3})", &st), "再次解析成功");
    check(std::string(st.raw).find("\"data\":2") != std::string::npos,
          "raw 保留最近一次输入");

    // 每个信息源独立: 喂 source 2 不应影响 source 1
    firecontrol::json_status_init();
    const firecontrol::JsonStatus s0 = firecontrol::json_status_for(0);
    check(s0.has_frame, "信息源 1 默认有完整帧");
    const firecontrol::JsonStatus s1_empty = firecontrol::json_status_for(1);
    check(!s1_empty.has_frame, "信息源 2 未喂数据前没有完整帧");

    const char *source2_frame = R"({"start":1}{"data":7,"smoke":1}{"end":1})";
    check(firecontrol::json_source_feed(1, source2_frame,
                                        std::strlen(source2_frame)) == 1,
          "喂给信息源 2 一个完整帧");
    const firecontrol::JsonStatus s1 = firecontrol::json_status_for(1);
    check(s1.has_frame && s1.valid && s1.data == 7 && s1.smoke == 1,
          "信息源 2 的解析结果正确");
    check(firecontrol::json_status_for(0).frame == s0.frame,
          "信息源 1 不受信息源 2 影响");

    const char *partial = R"({"start":1}{"data":8)";
    check(firecontrol::json_source_feed(1, partial, std::strlen(partial)) == 0,
          "不完整帧不会发布到信息源 2");
    firecontrol::json_source_reset(1);
    check(!firecontrol::json_status_for(1).has_frame,
          "reset 后信息源 2 恢复空状态");

    if (g_failures != 0) {
        std::cout << "[FAIL] 共 " << g_failures << " 项失败\n";
        return 1;
    }

    std::cout << "[ OK ] 全部通过\n";
    return 0;
}
