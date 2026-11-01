#include "json_status.h"

#include "cJSON.h"
#include "json_stream.h"

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
    R"({"header":"SENSOR_DATA","data":{"temperature":20.06,"int_dev":1,)"
    R"("int_err":0,"fire_alarm":0,"smoke_alarm":0,"led_state":0,)"
    R"("buzzer_state":0},"footer":"END"})";

constexpr const char *kErrEmpty = "JSON 为空";
constexpr const char *kErrFileOpen = "打不开 JSON 文件";
constexpr const char *kErrFileRead = "读 JSON 文件失败";
constexpr const char *kErrSyntax = "JSON 语法错误";
constexpr const char *kErrNotObject = "根节点不是对象";
constexpr const char *kErrNoFields = "没有已知字段";
constexpr const char *kErrDataBadType = "data 不是对象/数字";
constexpr const char *kErrSmokeNotNumber = "smoke 不是数字";
constexpr const char *kErrTemperatureNotNumber = "temperature 不是数字";
constexpr const char *kErrFireStateNotNumber = "fire_state 不是数字";
constexpr const char *kErrSmokeStateNotNumber = "smoke_state 不是数字";
constexpr const char *kErrSmokeAlarmNotNumber = "smoke_alarm 不是数字";
constexpr const char *kErrBuzzerStateNotNumber = "buzzer_state 不是数字";
constexpr const char *kErrLedStateNotNumber = "led_state 不是数字";
// 旧字段名 (别名) 的类型错误分开写, 报错时能和实际发的键对上
constexpr const char *kErrFireAlarmNotNumber = "fire_alarm 不是数字";
constexpr const char *kErrSmokerAlarmNotNumber = "smoker_alarm 不是数字";
constexpr const char *kErrBuzzerAlarmNotNumber = "buzzer_alarm 不是数字";

// 每个信息源一路独立输入。串口读取线程调 json_source_feed() 喂字节,
// 文件来源 (测试用) 也走同一套 splitter/status。
//
// **不再有 start/end 切帧**: 模块自己带完整报文, app 只从字节流里抠出
// `{...}` 完整 JSON 对象 (见 json_stream.h)。
struct SourceState {
    JsonStatus status;
    JsonObjectSplitter splitter;
    std::string file_path;
    std::string last_file_content;
    bool from_file = false;
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
           a.has_fire_alarm == b.has_fire_alarm && a.fire_alarm == b.fire_alarm &&
           a.has_smoke_alarm == b.has_smoke_alarm &&
           a.smoke_alarm == b.smoke_alarm &&
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

// 解析这一路切出来的每条完整对象; 返回发布了几条。
//
// 模块自己的报文头/尾 (比如 {"start":1}) 本身也是合法 JSON 对象, 但里面没有已知
// 字段 —— 这种**静默跳过**, 不往界面上报"未识别"。
int processObjects(SourceState &src)
{
    int count = 0;
    std::string object;
    while (src.splitter.pop(object)) {
        JsonStatus st;
        json_parse(object.c_str(), &st);
        if (!st.valid && sameError(st.error, kErrNoFields)) {
            continue;
        }
        st.has_frame = true;
        st.frame = object;
        publish(src, std::move(st), true);
        count++;
    }
    return count;
}

int feedWholeText(SourceState &src, const std::string &text)
{
    src.splitter.reset();
    src.splitter.feed(text);
    return processObjects(src) > 0 ? 0 : -1;
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
    return feedWholeText(src, text);
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
        return feedWholeText(src, text);
    }

    if (text == src.last_file_content) {
        return 0;
    }

    // 追加 (新内容以旧内容为前缀): 只喂新增后缀, 跨刷新被切开的半个对象能接上。
    if (text.size() >= src.last_file_content.size() &&
        text.compare(0, src.last_file_content.size(), src.last_file_content) == 0) {
        const std::size_t old_size = src.last_file_content.size();
        src.last_file_content = text;
        src.splitter.feed(text.data() + old_size, text.size() - old_size);
        return processObjects(src) > 0 ? 0 : -1;
    }

    // 重写: 重置后整段重喂。
    src.last_file_content = text;
    return feedWholeText(src, text);
}

void resetSource(SourceState &src)
{
    src.status = JsonStatus{};
    src.splitter.reset();
    src.file_path.clear();
    src.last_file_content.clear();
    src.from_file = false;
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

    // 报文信封: {"header":"SENSOR_DATA","data":{...},"footer":"END"}\r\n。
    // 有 data 对象就只从它里面取字段; 没有则按旧的扁平格式从根对象取。
    // data 是数字时是更早的格式 {"data":1,"smoke":1}, 照旧认。
    const cJSON *body = root;
    {
        const cJSON *data_item = cJSON_GetObjectItemCaseSensitive(root, "data");
        if (data_item != nullptr) {
            if (cJSON_IsObject(data_item)) {
                body = data_item;
            } else if (cJSON_IsNumber(data_item)) {
                out->has_data = true;
                out->data = static_cast<int>(data_item->valuedouble);
            } else {
                return fail(kErrDataBadType);
            }
        }
    }

    // 已知字段全是可选的: 缺字段不算错 (模块可能只发它有的那几个),
    // 存在但不是数字才算失败。
    bool ok = true;
    auto readInt = [&](const char *name, bool *has, int *value, const char *type_error) {
        if (!ok) {
            return;
        }
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(body, name);
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

    // 模块现在用的键名优先; 旧键名作别名兜底 (字段全部可选, 谁先出现用谁)。
    readInt("smoke", &out->has_smoke, &out->smoke, kErrSmokeNotNumber);
    readInt("fire_alarm", &out->has_fire_alarm, &out->fire_alarm,
            kErrFireAlarmNotNumber);
    if (ok && !out->has_fire_alarm) {
        readInt("fire_state", &out->has_fire_alarm, &out->fire_alarm,
                kErrFireStateNotNumber);
    }
    readInt("smoke_alarm", &out->has_smoke_alarm, &out->smoke_alarm,
            kErrSmokeAlarmNotNumber);
    if (ok && !out->has_smoke_alarm) {
        readInt("smoker_alarm", &out->has_smoke_alarm, &out->smoke_alarm,
                kErrSmokerAlarmNotNumber);
    }
    if (ok && !out->has_smoke_alarm) {
        readInt("smoke_state", &out->has_smoke_alarm, &out->smoke_alarm,
                kErrSmokeStateNotNumber);
    }
    readInt("buzzer_state", &out->has_buzzer_state, &out->buzzer_state,
            kErrBuzzerStateNotNumber);
    if (ok && !out->has_buzzer_state) {
        readInt("buzzer_alarm", &out->has_buzzer_state, &out->buzzer_state,
                kErrBuzzerAlarmNotNumber);
    }
    readInt("led_state", &out->has_led_state, &out->led_state,
            kErrLedStateNotNumber);

    if (ok) {
        const cJSON *temperature =
            cJSON_GetObjectItemCaseSensitive(body, "temperature");
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
        out->has_fire_alarm || out->has_smoke_alarm || out->has_buzzer_state ||
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
        rc = feedWholeText(g_sources[0], literal);
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
            rc = feedWholeText(g_sources[0], kSampleFrame);
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
    src.splitter.feed(data, len);
    return processObjects(src);
}

void json_source_reset(int source_index)
{
    if (source_index < 0 || source_index >= kInfoSourceCount) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    SourceState &src = g_sources[source_index];
    src.splitter.reset();
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
