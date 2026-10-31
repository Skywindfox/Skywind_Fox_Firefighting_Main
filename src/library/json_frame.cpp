#include "json_frame.h"

#include <utility>

namespace firecontrol
{
void JsonFrameParser::reset()
{
    buffer_.clear();
    frames_.clear();
    in_frame_ = false;
}

void JsonFrameParser::feed(const char *data, std::size_t len)
{
    if (data == nullptr || len == 0) {
        return;
    }
    buffer_.append(data, len);
    process();
}

void JsonFrameParser::feed(const std::string &text)
{
    feed(text.data(), text.size());
}

bool JsonFrameParser::pop(std::string &frame)
{
    if (frames_.empty()) {
        return false;
    }
    frame = std::move(frames_.front());
    frames_.pop_front();
    return true;
}

bool JsonFrameParser::collecting() const
{
    return in_frame_;
}

void JsonFrameParser::process()
{
    const std::string start(kFrameStart);
    const std::string end(kFrameEnd);

    for (;;) {
        if (!in_frame_) {
            const std::size_t pos = buffer_.find(start);
            if (pos == std::string::npos) {
                // 保留可能是 start 前缀的尾巴, 其余垃圾丢掉。
                if (buffer_.size() > start.size() - 1) {
                    buffer_.erase(0, buffer_.size() - (start.size() - 1));
                }
                return;
            }
            if (pos > 0) {
                buffer_.erase(0, pos);
            }
            in_frame_ = true;
        }

        // 在等 end 的过程中又出现新的 start: 旧帧不完整, 丢弃它,
        // 从新的 start 重新收集。
        const std::size_t end_pos = buffer_.find(end, start.size());
        const std::size_t next_start = buffer_.find(start, start.size());
        if (next_start != std::string::npos &&
            (end_pos == std::string::npos || next_start < end_pos)) {
            buffer_.erase(0, next_start);
            continue;
        }

        if (end_pos == std::string::npos) {
            // 还没等到 end。超过上限就丢掉当前 start, 从后面的 start 重新找。
            if (buffer_.size() > kFrameMaxBytes) {
                buffer_.erase(0, start.size());
                in_frame_ = false;
                continue;
            }
            return;
        }

        const std::size_t frame_len = end_pos + end.size();
        frames_.push_back(buffer_.substr(0, frame_len));
        buffer_.erase(0, frame_len);
        in_frame_ = false;
    }
}
}
