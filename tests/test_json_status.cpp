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

    // 旧的最小示例: 仍然认
    check(parse(R"({"data":1,"smoke":1})", &st), "解析 {\"data\":1,\"smoke\":1}");
    check(st.valid, "valid = true");
    check(st.has_data && st.has_smoke, "data / smoke 都存在");
    check(st.data == 1 && st.smoke == 1, "data=1, smoke=1");
    check(st.error == nullptr, "成功时没有错误");

    // 模块固件的五个字段 (字段名与固件变量一致)
    check(parse(R"({"temperature":36.5,"fire_state":1,"smoke_state":0,"buzzer_state":2,"led_state":4})",
                &st),
          "解析模块的五个字段");
    check(st.valid && st.has_temperature && st.temperature == 36.5,
          "temperature=36.5");
    check(st.has_fire_state && st.fire_state == 1, "fire_state=1 (有火)");
    check(st.has_smoke_state && st.smoke_state == 0, "smoke_state=0 (无烟)");
    check(st.has_buzzer_state && st.buzzer_state == 2, "buzzer_state=2 (缓慢)");
    check(st.has_led_state && st.led_state == 4, "led_state=4 (蓝+绿)");
    check(!st.has_data && !st.has_smoke, "没有 data/smoke 也不算错");

    // 旧协议名作别名: fire_alarm / smoker_alarm / buzzer_alarm
    check(parse(R"({"fire_alarm":1,"smoker_alarm":1,"buzzer_alarm":3})", &st),
          "旧字段名作别名仍可解析");
    check(st.fire_state == 1 && st.smoke_state == 1 && st.buzzer_state == 3,
          "别名映射到同一组状态字段");

    // 固件名优先于别名
    check(parse(R"({"fire_state":0,"fire_alarm":1})", &st), "两个键同时出现也能解析");
    check(st.fire_state == 0, "fire_state 优先");

    // 整数温度 / 只有部分字段
    check(parse(R"({"temperature":30,"fire_state":0})", &st), "整数温度可以解析");
    check(st.temperature == 30.0, "temperature=30");
    check(st.has_fire_state && !st.has_smoke_state && !st.has_buzzer_state &&
              !st.has_led_state,
          "缺的字段 has_* 是 false, 不算失败");

    // 字段类型不对 → 失败, 并说明是哪个字段
    check(!parse(R"({"temperature":"hot","fire_state":1})", &st),
          "temperature 是字符串时失败");
    check(!st.valid && st.error != nullptr &&
              std::string(st.error).find("temperature") != std::string::npos,
          "错误信息提到 temperature");

    check(!parse(R"({"fire_state":"yes"})", &st), "fire_state 是字符串时失败");
    check(!st.valid && st.error != nullptr &&
              std::string(st.error).find("fire_state") != std::string::npos,
          "错误信息提到 fire_state");

    check(!parse(R"({"smoke_state":null})", &st), "smoke_state 是 null 时失败");
    check(!parse(R"({"buzzer_state":"x"})", &st), "buzzer_state 类型不对时失败");
    check(!parse(R"({"led_state":true})", &st), "led_state 是布尔时失败");

    // 旧格式的字段类型检查还在
    check(!parse(R"({"data":"1","smoke":1})", &st), "data 是字符串时失败");
    check(!st.valid, "类型错误时 valid = false");

    // 一个已知字段都没有: 帧收到但没数据可用, 明确报错
    check(!parse("{}", &st), "空对象失败");
    check(!st.valid && st.error != nullptr &&
              std::string(st.error).find("没有已知字段") != std::string::npos,
          "错误信息是\"没有已知字段\"");
    check(!parse(R"({"foo":1,"bar":2})", &st), "只有未知字段时失败");

    // 空格、换行、额外字段
    check(parse("{\n  \"temperature\": 21.5,\n  \"extra\": \"ignored\"\n}", &st),
          "解析带空白/额外字段的对象");
    check(st.valid && st.temperature == 21.5, "额外字段被忽略");

    // 语法错误 / 根节点不是对象 / 空输入 / 尾部垃圾
    check(!parse("{", &st), "语法错误时失败");
    check(!st.valid, "语法错误时 valid = false");

    check(!parse("[1,2,3]", &st), "根节点是数组时失败");
    check(!st.valid, "数组时 valid = false");

    check(!parse("", &st), "空字符串失败");
    check(!st.valid, "空字符串时 valid = false");

    check(!parse(R"({"data":1,"smoke":1} trailing)", &st), "JSON 后面有垃圾时失败");
    check(!st.valid, "尾部垃圾时 valid = false");

    // 原文保留 (用于界面/排查)
    check(parse(R"({"temperature":2.5,"fire_state":0})", &st), "再次解析成功");
    check(std::string(st.raw).find("\"temperature\":2.5") != std::string::npos,
          "raw 保留最近一次输入");

    // 每个信息源独立: 喂 source 2 不应影响 source 1
    firecontrol::json_status_init();
    const firecontrol::JsonStatus s0 = firecontrol::json_status_for(0);
    check(s0.has_frame, "信息源 1 默认有完整帧 (内置示例)");
    check(s0.valid && s0.has_temperature && s0.has_fire_state && s0.has_led_state,
          "内置示例帧带模块字段");

    const firecontrol::JsonStatus s1_empty = firecontrol::json_status_for(1);
    check(!s1_empty.has_frame, "信息源 2 未喂数据前没有完整帧");

    const char *source2_frame =
        R"({"start":1}{"temperature":36.5,"fire_state":1,"smoke_state":0,"buzzer_state":0,"led_state":3}{"end":1})";
    check(firecontrol::json_source_feed(1, source2_frame,
                                        std::strlen(source2_frame)) == 1,
          "喂给信息源 2 一个完整帧");
    const firecontrol::JsonStatus s1 = firecontrol::json_status_for(1);
    check(s1.has_frame && s1.valid && s1.temperature == 36.5 && s1.fire_state == 1 &&
              s1.led_state == 3,
          "信息源 2 的解析结果正确");
    check(firecontrol::json_status_for(0).frame == s0.frame,
          "信息源 1 不受信息源 2 影响");

    const char *partial = R"({"start":1}{"temperature":8)";
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
