// test_json_stream.cpp — 字节流里切 JSON 对象的单元测试
//
// 模块自己带完整报文, app 不再要求 {"start":1}/{"end":1}:
// 只认 `{...}`, 对象之外的字节 (模块的头/尾、垃圾) 全部丢弃。
//
// 重点验证: 分块到达、对象被拆开、多条对象、头尾垃圾、字符串/嵌套/转义、
// 超长对象丢弃后能恢复。

#include "json_stream.h"

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

std::vector<std::string> feed(firecontrol::JsonObjectSplitter &splitter,
                              const std::string &text)
{
    splitter.feed(text);
    std::vector<std::string> objects;
    std::string object;
    while (splitter.pop(object)) {
        objects.push_back(object);
    }
    return objects;
}

} // namespace

int main()
{
    std::cout << "FireControlApp JSON 对象切分单元测试:\n";

    {
        firecontrol::JsonObjectSplitter s;
        auto objects = feed(s, R"({"temperature":36.5,"fire_state":1})");
        check(objects.size() == 1, "一个完整对象被切出来");
        check(objects.size() == 1 && objects[0] == R"({"temperature":36.5,"fire_state":1})",
              "切出的对象内容原样");
        check(!s.collecting(), "完整对象之后不在收集状态");
    }

    {
        firecontrol::JsonObjectSplitter s;
        check(feed(s, R"({"temperature":3)").empty(), "只有左半边时不出对象");
        check(s.collecting(), "正在等右括号");
        auto objects = feed(s, R"(6.5,"fire_state":0})");
        check(objects.size() == 1, "补齐右括号后出对象");
        check(objects.size() == 1 && objects[0] == R"({"temperature":36.5,"fire_state":0})",
              "跨块拼出的对象内容正确");
    }

    {
        firecontrol::JsonObjectSplitter s;
        auto objects = feed(s, R"({"a":1}{"b":2})");
        check(objects.size() == 2, "一次收到两条对象");
        check(objects.size() == 2 && objects[0] == R"({"a":1})" &&
                  objects[1] == R"({"b":2})",
              "两条对象的内容和顺序正确");
    }

    {
        firecontrol::JsonObjectSplitter s;
        // 模块自己的报文头/尾、换行、垃圾都在对象外面
        auto objects = feed(s, std::string("AA 55 0D 0A ") + R"({"buzzer_state":2})" +
                                   " \r\n\x02\x03");
        check(objects.size() == 1, "头尾垃圾被跳过");
        check(objects.size() == 1 && objects[0] == R"({"buzzer_state":2})",
              "对象内容不受头尾影响");
        check(!s.collecting(), "垃圾不会让 splitter 卡在收集状态");
    }

    {
        firecontrol::JsonObjectSplitter s;
        // 字符串里的花括号 / 转义引号 不能被当成对象边界
        auto objects = feed(s, R"({"note":"{not a brace}","q":"a\"b"}{"x":1})");
        check(objects.size() == 2, "字符串里的括号不误切");
        check(objects.size() == 2 && objects[0] ==
                  R"({"note":"{not a brace}","q":"a\"b"})",
              "字符串内容原样保留");
    }

    {
        firecontrol::JsonObjectSplitter s;
        auto objects = feed(s, R"({"a":{"b":{"c":1}}})");
        check(objects.size() == 1, "嵌套对象按最外层闭合");
        check(objects.size() == 1 && objects[0] == R"({"a":{"b":{"c":1}}})",
              "嵌套对象内容完整");
    }

    {
        firecontrol::JsonObjectSplitter s;
        // 超过上限的半个对象: 丢掉, 再从后面的 '{' 重新开始
        std::string huge = "{\"pad\":\"" + std::string(9000, 'x');
        check(feed(s, huge).empty(), "超长未闭合对象不产出");
        auto objects = feed(s, R"(}}garbage{"ok":1})");
        check(objects.size() == 1, "超长对象丢弃后能从下一个 '{' 恢复");
        check(objects.size() == 1 && objects[0] == R"({"ok":1})", "恢复后的对象正确");
    }

    {
        firecontrol::JsonObjectSplitter s;
        s.feed(R"({"half":1)");
        s.reset();
        check(!s.collecting(), "reset 清掉半个对象");
        auto objects = feed(s, R"({"restart":1})");
        check(objects.size() == 1 && objects[0] == R"({"restart":1})",
              "reset 后可以重新开始");
    }

    {
        firecontrol::JsonObjectSplitter s;
        check(feed(s, "no json here at all").empty(), "没有 '{' 时不产出");
        check(!s.collecting(), "没有 '{' 时不在收集状态");
    }

    if (g_failures != 0) {
        std::cout << "[FAIL] 共 " << g_failures << " 项失败\n";
        return 1;
    }

    std::cout << "[ OK ] 全部通过\n";
    return 0;
}
