#include "json_status.h"

#include "cJSON.h"
#include "json_frame.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

#include <unistd.h>

namespace firecontrol
{
namespace
{
constexpr const char *kSampleFrame =
    R"({"start":1}{"temperature":25.5,"fire_state":0,"smoke_state":0,)"
    R"("buzzer_state":0,"led_state":0}{"end":1})";

constexpr const char *kErrEmpty = "JSON 为空";
constexpr const char *kErrFileOpen = "打不开 JSON 文件";
constexpr const char *kErrFileRead = "读 JSON 文件失败";
constexpr const char *kErrSyntax = "JSON 语法错误";
constexpr const char *kErrNotObject = "根节点不是对象";
constexpr const char *kErrNoFields = "没有已知字段";
constexpr const char *kErrDataNotNumber = "data 不是数字";
constexpr const char *kErrSmokeNotNumber = "smoke 不是数字";
constexpr const char *kErrTemperatureNotNumber = "temperature 不是数字";
constexpr const char *kErrFireStateNotNumber = "fire_state 不是数字";
constexpr const char *kErrSmokeStateNotNumber = "smoke_state 不是数字";
constexpr const char *kErrBuzzerStateNotNumber = "buzzer_state 不是数字";
constexpr const char *kErrLedStateNotNumber = "led_state 不是数字";
// 旧字段名 (别名) 的类型错误分开写, 报错时能和实际发的键对上
constexpr const char *kErrFireAlarmNotNumber = "fire_alarm 不是数字";
constexpr const char *kErrSmokerAlarmNotNumber = "smoker_alarm 不是数字";
constexpr const char *kErrBuzzerAlarmNotNumber = "buzzer_alarm 不是数字";

// 每个信息源一路独立输入。串口读取线程调 json_source_feed() 喂字节,
// 文件来源 (测试用) 也走同一套 parser/status。
struct SourceState {
    JsonStatus status;
    JsonFrameParser parser;
    std::string file_path;
    std::string last_file_content;
    bool from_file = false;
    bool framed_mode = false;
    bool file_error = false;
};

SourceState g_sources[kInfoSourceCount];
std::mutex g_mutex;

bool sameError(const char *a, const char *b)
{
    if (a == nullptr || b == nullptr) {
        return a == b;
    }
    return std::strcmp(a, b) == 0;
}

bool sameValues(const JsonStatus &a, const JsonStatus &b)
{
    return a.valid == b.valid && a.has_data == b.has_data &&
           a.has_smoke == b.has_smoke && a.data == b.data && a.smoke == b.smoke &&
           a.has_temperature == b.has_temperature && a.temperature == b.temperature &&
           a.has_fire_state == b.has_fire_state && a.fire_state == b.fire_state &&
           a.has_smoke_state == b.has_smoke_state &&
           a.smoke_state == b.smoke_state &&
           a.has_buzzer_state == b.has_buzzer_state &&
           a.buzzer_state == b.buzzer_state &&
           a.has_led_state == b.has_led_state && a.led_state == b.led_state &&
           sameError(a.error, b.error) && a.has_frame == b.has_frame &&
           a.frame == b.frame;
}

void publish(SourceState &src, JsonStatus next, bool force = false)
{
    if (force || !src.status.revision || !sameValues(src.status, next)) {
        next.revision = src.status.revision + 1;
        src.status = std::move(next);
        return;
    }
    std::snprintf(src.status.raw, sizeof src.status.raw, "%s", next.raw);
}

int readTextFile(const char *path, std::string *out, const char **error)
{
    if (out == nullptr) {
        return -1;
    }
    out->clear();
    if (path == nullptr || *path == '\0') {
        if (error != nullptr) {
            *error = kErrFileOpen;
        }
        return -1;
    }

    std::FILE *fp = std::fopen(path, "rb");
    if (fp == nullptr) {
        if (error != nullptr) {
            *error = kErrFileOpen;
        }
        return -1;
    }

    char buf[512];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) {
        out->append(buf, n);
    }
    const bool read_failed = std::ferror(fp) != 0;
    std::fclose(fp);

    if (read_failed) {
        if (error != nullptr) {
            *error = kErrFileRead;
        }
        return -1;
    }
    return 0;
}

bool hasStartMarker(const std::string &text)
{
    return text.find(kFrameStart) != std::string::npos;
}

std::string extractPayload(const std::string &frame)
{
    const std::string start(kFrameStart);
    const std::string end(kFrameEnd);
    const std::size_t s = frame.find(start);
    const std::size_t e = frame.rfind(end);
    if (s == std::string::npos || e == std::string::npos ||
        e < s + start.size()) {
        return {};
    }
    return frame.substr(s + start.size(), e - (s + start.size()));
}

int processFrames(SourceState &src)
{
    int count = 0;
    std::string frame;
    while (src.parser.pop(frame)) {
        JsonStatus st;
        const std::string payload = extractPayload(frame);
        json_parse(payload.c_str(), &st);
        st.has_frame = true;
        st.frame = frame;
        publish(src, std::move(st), true);
        count++;
    }
    return count;
}

int applyLegacyText(SourceState &src, const std::string &text)
{
    JsonStatus st;
    const int rc = json_parse(text.c_str(), &st);
    st.has_frame = true;
    st.frame = text;
    publish(src, std::move(st), true);
    return rc;
}

int feedWholeText(SourceState &src, const std::string &text, bool allow_legacy)
{
    src.parser.reset();
    if (hasStartMarker(text)) {
        src.framed_mode = true;
        src.parser.feed(text);
        return processFrames(src) > 0 ? 0 : -1;
    }
    if (allow_legacy) {
        src.framed_mode = false;
        return applyLegacyText(src, text);
    }
    return -1;
}

void publishFileError(SourceState &src, const char *error)
{
    JsonStatus st;
    st.error = error;
    st.has_frame = src.status.has_frame;
    st.frame = src.status.frame;
    publish(src, std::move(st), true);
}

int initSourceFromFile(SourceState &src, const char *path)
{
    src.from_file = true;
    src.file_path = path;
    std::string text;
    const char *error = nullptr;
    if (readTextFile(path, &text, &error) != 0) {
        src.file_error = true;
        publishFileError(src, error);
        return -1;
    }
    src.file_error = false;
    src.last_file_content = text;
    return feedWholeText(src, text, true);
}

int refreshSourceFile(SourceState &src)
{
    if (!src.from_file) {
        return 0;
    }

    std::string text;
    const char *error = nullptr;
    if (readTextFile(src.file_path.c_str(), &text, &error) != 0) {
        src.file_error = true;
        publishFileError(src, error);
        return -1;
    }

    if (src.file_error) {
        src.file_error = false;
        src.last_file_content = text;
        return feedWholeText(src, text, !src.framed_mode);
    }

    if (text == src.last_file_content) {
        return 0;
    }

    if (src.framed_mode && hasStartMarker(text) &&
        text.size() >= src.last_file_content.size() &&
        text.compare(0, src.last_file_content.size(), src.last_file_content) == 0) {
        const std::size_t old_size = src.last_file_content.size();
        src.last_file_content = text;
        src.parser.feed(text.data() + old_size, text.size() - old_size);
        return processFrames(src) > 0 ? 0 : -1;
    }

    const bool allow_legacy = !src.framed_mode;
    src.last_file_content = text;
    return feedWholeText(src, text, allow_legacy);
}

void resetSource(SourceState &src)
{
    src.status = JsonStatus{};
    src.parser.reset();
    src.file_path.clear();
    src.last_file_content.clear();
    src.from_file = false;
    src.framed_mode = false;
    src.file_error = false;
}
} // namespace

int json_parse(const char *text, JsonStatus *out)
{
    if (out == nullptr) {
        return -1;
    }

    *out = JsonStatus{};
    if (text == nullptr || *text == '\0') {
        out->error = kErrEmpty;
        return -1;
    }

    std::snprintf(out->raw, sizeof out->raw, "%s", text);

    cJSON *root = cJSON_ParseWithOpts(text, nullptr, 1);
    if (root == nullptr) {
        out->error = kErrSyntax;
        return -1;
    }

    auto fail = [&](const char *reason) {
        cJSON_Delete(root);
        out->valid = false;
        out->error = reason;
        return -1;
    };

    if (!cJSON_IsObject(root)) {
        return fail(kErrNotObject);
    }

    // 已知字段全是可选的: 缺字段不算错 (模块可能只发它有的那几个),
    // 存在但不是数字才算失败。
    bool ok = true;
    auto readInt = [&](const char *name, bool *has, int *value, const char *type_error) {
        if (!ok) {
            return;
        }
        cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
        if (item == nullptr) {
            return;
        }
        if (!cJSON_IsNumber(item)) {
            out->error = type_error;
            ok = false;
            return;
        }
        *has = true;
        *value = static_cast<int>(item->valuedouble);
    };

    // 同一个字段两个键名: 模块固件的名字优先, 旧协议名作别名兜底。
    auto readIntFirst = [&](const char *name, const char *alias, bool *has, int *value,
                            const char *name_error, const char *alias_error) {
        readInt(name, has, value, name_error);
        if (ok && !*has) {
            readInt(alias, has, value, alias_error);
        }
    };

    readInt("data", &out->has_data, &out->data, kErrDataNotNumber);
    readInt("smoke", &out->has_smoke, &out->smoke, kErrSmokeNotNumber);
    readIntFirst("fire_state", "fire_alarm", &out->has_fire_state, &out->fire_state,
                 kErrFireStateNotNumber, kErrFireAlarmNotNumber);
    readIntFirst("smoke_state", "smoker_alarm", &out->has_smoke_state,
                 &out->smoke_state, kErrSmokeStateNotNumber,
                 kErrSmokerAlarmNotNumber);
    readIntFirst("buzzer_state", "buzzer_alarm", &out->has_buzzer_state,
                 &out->buzzer_state, kErrBuzzerStateNotNumber,
                 kErrBuzzerAlarmNotNumber);
    readInt("led_state", &out->has_led_state, &out->led_state, kErrLedStateNotNumber);

    if (ok) {
        cJSON *temperature = cJSON_GetObjectItemCaseSensitive(root, "temperature");
        if (temperature != nullptr) {
            if (!cJSON_IsNumber(temperature)) {
                out->error = kErrTemperatureNotNumber;
                ok = false;
            } else {
                out->has_temperature = true;
                out->temperature = temperature->valuedouble;
            }
        }
    }
    if (!ok) {
        return fail(out->error);
    }

    // 一个已知字段都没有: 帧收到了但里面没有能用的数据, 明确报出来,
    // 不要假装 valid。
    const bool any_known =
        out->has_data || out->has_smoke || out->has_temperature ||
        out->has_fire_state || out->has_smoke_state || out->has_buzzer_state ||
        out->has_led_state;
    if (!any_known) {
        return fail(kErrNoFields);
    }

    out->valid = true;
    out->error = nullptr;

    cJSON_Delete(root);
    return 0;
}

int json_load_file(const char *path, JsonStatus *out)
{
    if (out == nullptr) {
        return -1;
    }

    *out = JsonStatus{};
    std::string text;
    const char *error = nullptr;
    if (readTextFile(path, &text, &error) != 0) {
        out->error = error;
        return -1;
    }
    return json_parse(text.c_str(), out);
}

int json_status_init()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (SourceState &src : g_sources) {
        resetSource(src);
    }

    int rc = -1;

    const char *literal = std::getenv("FIRECONTROL_JSON");
    if (literal != nullptr && *literal != '\0') {
        rc = feedWholeText(g_sources[0], literal, true);
    } else {
        const char *file_env = std::getenv("FIRECONTROL_JSON_FILE");
        const bool explicit_file = (file_env != nullptr && *file_env != '\0');
        const char *path = explicit_file ? file_env : "firecontrol.json";

        if (access(path, R_OK) == 0) {
            rc = initSourceFromFile(g_sources[0], path);
        } else if (explicit_file) {
            g_sources[0].from_file = true;
            g_sources[0].file_path = path;
            g_sources[0].file_error = true;
            publishFileError(g_sources[0], kErrFileOpen);
            rc = -1;
        } else {
            rc = feedWholeText(g_sources[0], kSampleFrame, true);
        }
    }

    // 信息源 1..11: 每路独立输入。现在还没接串口, 先用可选的
    // FIRECONTROL_JSON_FILE_<N> 文件做测试; 没设置就保持空状态。
    for (int i = 1; i < kInfoSourceCount; i++) {
        char env_name[40] = {};
        std::snprintf(env_name, sizeof env_name, "FIRECONTROL_JSON_FILE_%d", i + 1);
        const char *path = std::getenv(env_name);
        if (path != nullptr && *path != '\0') {
            initSourceFromFile(g_sources[i], path);
        }
    }

    return rc;
}

int json_status_refresh()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    int rc = 0;
    for (int i = 0; i < kInfoSourceCount; i++) {
        const int r = refreshSourceFile(g_sources[i]);
        if (i == 0 && r < 0) {
            rc = -1;
        }
    }
    return rc;
}

int json_source_feed(int source_index, const char *data, std::size_t len)
{
    if (source_index < 0 || source_index >= kInfoSourceCount ||
        data == nullptr || len == 0) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    SourceState &src = g_sources[source_index];
    src.parser.feed(data, len);
    return processFrames(src);
}

void json_source_reset(int source_index)
{
    if (source_index < 0 || source_index >= kInfoSourceCount) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    SourceState &src = g_sources[source_index];
    src.parser.reset();
    JsonStatus empty;
    empty.revision = src.status.revision + 1;
    src.status = std::move(empty);
}

JsonStatus json_status_for(int source_index)
{
    if (source_index < 0 || source_index >= kInfoSourceCount) {
        return JsonStatus{};
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    return g_sources[source_index].status;
}

JsonStatus json_status()
{
    return json_status_for(0);
}
} // namespace firecontrol
