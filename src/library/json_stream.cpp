#include "json_stream.h"

#include <utility>

namespace firecontrol
{
void JsonObjectSplitter::reset()
{
    buffer_.clear();
    objects_.clear();
    collecting_ = false;
}

void JsonObjectSplitter::feed(const char *data, std::size_t len)
{
    if (data == nullptr || len == 0) {
        return;
    }
    buffer_.append(data, len);
    process();
}

void JsonObjectSplitter::feed(const std::string &text)
{
    feed(text.data(), text.size());
}

bool JsonObjectSplitter::pop(std::string &object)
{
    if (objects_.empty()) {
        return false;
    }
    object = std::move(objects_.front());
    objects_.pop_front();
    return true;
}

bool JsonObjectSplitter::collecting() const
{
    return collecting_;
}

void JsonObjectSplitter::process()
{
    for (;;) {
        if (!collecting_) {
            const std::size_t start = buffer_.find('{');
            if (start == std::string::npos) {
                // 还没遇到对象的开头: 整段都是模块头/垃圾, 丢掉。
                buffer_.clear();
                return;
            }
            if (start > 0) {
                buffer_.erase(0, start);
            }
            collecting_ = true;
        }

        // 从 buffer_[0] 的 '{' 开始找配对的 '}'。字符串里的括号不算;
        // 反斜杠转义的下一个字符也不算。
        int depth = 0;
        bool in_string = false;
        bool escaped = false;
        std::size_t end = std::string::npos;

        for (std::size_t i = 0; i < buffer_.size(); i++) {
            const char c = buffer_[i];
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    in_string = false;
                }
                continue;
            }
            if (c == '"') {
                in_string = true;
            } else if (c == '{') {
                depth++;
            } else if (c == '}') {
                depth--;
                if (depth == 0) {
                    end = i;
                    break;
                }
            }
        }

        if (end == std::string::npos) {
            // 还没闭合: 超过上限就丢掉这条, 从下一个 '{' 重新找。
            if (buffer_.size() > kJsonObjectMaxBytes) {
                const std::size_t next = buffer_.find('{', 1);
                if (next == std::string::npos) {
                    buffer_.clear();
                    collecting_ = false;
                } else {
                    buffer_.erase(0, next);
                }
                continue;
            }
            return;
        }

        objects_.push_back(buffer_.substr(0, end + 1));
        buffer_.erase(0, end + 1);
        collecting_ = false;
    }
}
} // namespace firecontrol
