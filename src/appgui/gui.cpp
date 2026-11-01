// gui.cpp — FireControlApp 主界面 (LVGL v9 + X11 后端)
//
// 目标平台: 正点原子 ATK-DLMP135 (STM32MP135) + Debian 12 + Xorg :0
//           屏幕 1024x600, 电容触摸屏
//
// 为什么用 LVGL 的 X11 后端, 而不是它更"嵌入式"的 framebuffer 后端:
//   板上跑着 Xorg, /dev/fb0 归 X server 管 —— 直接写 framebuffer 会和 Xorg 抢
//   同一块屏。X11 后端把绘制请求交给 X server, 部署方式一行都不用改; 触摸在
//   X11 里就是普通的鼠标事件, 键盘/滚轮也一并有驱动 (见 drivers/x11/)。
//   将来如果决定去掉 Xorg, 把这里的显示初始化换成 lv_linux_fbdev + lv_evdev
//   即可, 界面代码不用动。
//
// 中文怎么显示:
//   LVGL 开 FreeType, 运行时从板上的字体文件 (文泉驿正黑 wqy-zenhei.ttc) 取字形。
//   好处是改文案不用重新生成字库, 界面里出现任何汉字都能显示。板上本来就有
//   libfreetype (Xft 也在用), 没有引入新的运行时依赖。
//
// 本文件的历史与分工:
//   第一版由 Skywindfox 写: 裸 Xlib + Xft, 自己画矩形、自己算命中区域。
//   为什么用 Xft —— X11 核心位图字体只有 ISO-8859-1 编码, 画不出汉字, 中文会变
//   乱码; Xft 走 FreeType + fontconfig 才有完整 Unicode。
//   为什么不用 GTK/Qt —— 板子只有 437 MiB 内存, 它们空程序起步就是几十 MiB。
//   那一版的两个排查结论也都留在下面的实现里: 忽略窗口映射瞬间的杂散点击
//   (见 kIgnoreClicksMs), 以及字体必须确认真的含汉字字形。
//   当前这一版由 FBI-0537 重构到 LVGL —— 界面结构、字体加载、事件分发都换成
//   LVGL 那一套; "不用 GTK/Qt"这条理由对 LVGL 同样成立 (见 NOTES.md §8)。
//
// 编译: 见 CMakeLists.txt, 目标 deffire-gui-dev
// 运行:
//     ./deffire-gui-dev                 按屏幕尺寸建窗口 —— 板上用这个 (是否铺满由 WM 决定)
//     ./deffire-gui-dev --windowed      1024x600 窗口 —— 开发机预览用
//     ./deffire-gui-dev --font=<文件>   指定字体文件 (默认按候选表自动找)
//
// 界面结构 (分页):
//     首页            : 12 个信息源 (传感器模块) 的入口, 2 行 × 6 列
//     信息源 N 页     : 内容区"数据项"面板 (当前为空状态) + 居中【返回】
//     设置页          : LED / 蜂鸣器 / 外部无源蜂鸣器调试入口 + 返回
//     LED 调试页      : LED 开 / LED 关 / LED 心跳 / 返回
//     蜂鸣器调试页    : 蜂鸣器开 / 蜂鸣器关 / 蜂鸣器心跳 / 返回
//   顶栏 (品牌 / 状态灯 / 设置 / 退出) 所有页一致; 切页**只重建底栏按钮**
//   (见 g_page_dirty), 信息源页的数据面板在同一个 dirty 分支里显隐。
//   q / Esc 在每一页都是"退出程序", 不随页面变化 —— 板子上是触摸屏, 键盘只是开发机用。

#include <lvgl.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <clocale>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "json_status.h"
#include "serial_source.h"
#include "start.h"  // led_on / led_off (板载 sys-led)

namespace {

constexpr const char *kTitle = "FireControlApp";
constexpr const char *kText = "halloworld";

// 板上屏幕尺寸。仅用于窗口模式的默认值; 无参数时按实际屏幕尺寸建窗口。
constexpr int kBoardWidth = 1024;
constexpr int kBoardHeight = 600;

// 首页预留的信息源 (传感器模块) 数量。具体模块/数据项还没定, 所以只预留格子,
// 不预置任何传感器名字或数值 —— 点进去显示"尚未配置数据项"。
constexpr int kSensorCount = 12;
static_assert(kSensorCount == firecontrol::kInfoSourceCount,
              "UI 信息源数量必须与 json_status 的 kInfoSourceCount 一致");

// 信息源的传输方式: **每路一个独立串口设备** (设备路径 + 波特率), 数据是 JSON
// 字节流, 由 serial_source.cpp 的后台线程读进来、交给 json_source_feed() 切帧。
//
// 设备清单在运行时用 serial_list_devices() 枚举, 波特率用 serial_baud_rates();
// **这里不预置任何设备路径/波特率**, 也不猜 12 路默认映射 —— 用户在右栏选,
// 或者用 FIRECONTROL_SERIAL_<N>="/dev/ttyX:115200" 指定。
//
// 原来的"GPIO 接口 + 线号"下拉已经去掉: 那是**开关量**输入 (gpio_read_value,
// 0/1 电平) 的接法, 而模块给的是字节流, 两套东西不能混。板上那些开关量输入的
// 接线表仍在 start.cpp 的 kGpioInputs 里, 与本页无关。

// 字号 (像素)
constexpr uint32_t kBigFontPx = 40;     // 主标题
constexpr uint32_t kSmallFontPx = 18;   // 标题栏文字 / 按钮标签
constexpr uint32_t kStatusFontPx = 16;  // 状态说明这类"次要小字"

// ---------------------------------------------------------------------------
// 配色: 深色工业简约风
//
// 全部集中在这里 —— 控件代码里不出现颜色字面量, 想换风格只改这一段。
// 取值是按"触摸屏 + 机房/车间环境"挑的:
//   底压到近黑、文字提到近白 => 强光下也看得清, 夜间不刺眼;
//   强调色(安全琥珀)只出现在少数几个地方: 标题左侧竖条、主标题下划线、
//   状态灯之外的按下态描边 —— 目的是"一眼能找到当前可操作的位置"。
// ---------------------------------------------------------------------------
constexpr uint32_t kColBg         = 0x121417;  // 屏幕底
constexpr uint32_t kColPanel      = 0x1A1D21;  // 顶部标题栏 / 底部操作栏
constexpr uint32_t kColSurface    = 0x23272E;  // 按钮表面
constexpr uint32_t kColSeparator  = 0x2E343B;  // 描边 / 分隔线
constexpr uint32_t kColDividerDim = 0x24282E;  // 未实现按钮的描边(压暗)
constexpr uint32_t kColPressed    = 0x30363E;  // 按下态底色
constexpr uint32_t kColText       = 0xE6E8EA;  // 主文字
constexpr uint32_t kColTextDim    = 0x8A9199;  // 次要文字 / 未实现按钮
constexpr uint32_t kColAccent     = 0xFFB020;  // 强调色 (安全琥珀)
constexpr uint32_t kColOk         = 0x35C46A;  // 状态灯: 就绪/运行中

// 信息源接口页 (覆盖层) 的配色: 参考样图, 深蓝灰底 + 琥珀/橙描边。
// 只用于接口页, 不影响首页/设置/调试页的深色工业风。
constexpr uint32_t kSensorBg      = 0x0E141B;  // 页面底色: 深蓝黑, 不用满屏橙
constexpr uint32_t kSensorPanel   = 0x161D26;  // 侧栏/卡片底色
constexpr uint32_t kSensorSelected= 0x202A36;  // 当前信息源按钮
constexpr uint32_t kSensorLine    = 0x2A3644;  // 细描边/分隔线 (低对比)
constexpr uint32_t kSensorAccent  = 0xF2B544;  // 少量琥珀: 选中/标题/主按钮
constexpr uint32_t kSensorText    = 0xE8EDF2;  // 正文/按钮文字
constexpr uint32_t kSensorValue   = 0xFFFFFF;  // 数值
constexpr uint32_t kSensorDim     = 0x8B97A6;  // 占位/次要文字
constexpr uint32_t kSensorOk      = 0x4ADE80;  // 正常状态

// 启动后这段时间内的点击一律忽略。窗口刚映射时可能收到启动瞬间遗留的
// 杂散 ButtonPress, 不挡一下会"一启动就触发某个按钮"。
constexpr uint32_t kIgnoreClicksMs = 400;

// 主循环最长休眠时间, 防止 LVGL 给出的间隔异常时睡死。
constexpr uint32_t kMaxSleepMs = 10;

// 指针输入采样周期 (ms)。LVGL 默认把它建成 LV_DEF_REFR_PERIOD —— 但"画面多久
// 更新一次"和"多久看一眼有没有按下"是两件事。X11 后端的输入处理只保存最新状态
// (lv_x11_input.c: 按下置 true、松开置 false), 所以一次按下+松开如果落在同一个
// 采样窗口里, 这次点击**整次消失**。
// 实测 (2026-09-15, XTest 注入计时): 默认周期下按住 10ms 的快点击丢 60%;
// 采样压到 5ms 后同条件 0 丢失。详见 lv_conf.h 的 LV_DEF_REFR_PERIOD 注释。
constexpr uint32_t kIndevReadMs = 5;

// 单位换算: 布局按相对比例算, 换屏幕尺寸不用改代码。
constexpr int pct(int total, int percent) { return total * percent / 100; }

// ---------------------------------------------------------------------------
// 字体
//
// 按"文件路径"找字体, 而不是像 Xft 那样按 fontconfig 字体名 —— 名字查找会
// 被 fontconfig 静默替换成别的字体, 拿到一个不含汉字的字体也察觉不到。
// 给一串候选路径, 取第一个能打开的。候选表没命中时还会现场扫 /usr/share/fonts
// (见 scanForCjkFont) —— 硬编码的单个路径迟早会被发行版的包布局改掉。
//
// 板子上一共就这么几个可能的位置 (前两个是 Debian 官方包 fonts-wqy-zenhei 的
// 安装路径), 最后一个是 DejaVu —— 不含汉字, 只是保证程序还能跑起来。
// ---------------------------------------------------------------------------
const char *kFontCandidates[] = {
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
    "/usr/share/fonts/wqy-zenhei/wqy-zenhei.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/arphic/uming.ttc",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
};

bool fileReadable(const char *path)
{
    return access(path, R_OK) == 0;
}

// 忽略大小写的子串匹配 (strcasestr 是 GNU 扩展, 不写进可移植代码)。
bool containsIgnoreCase(const char *hay, const char *needle)
{
    for (const char *p = hay; *p != '\0'; p++) {
        const char *a = p;
        const char *b = needle;
        while (*a != '\0' && *b != '\0' &&
               std::tolower(static_cast<unsigned char>(*a)) ==
                   std::tolower(static_cast<unsigned char>(*b))) {
            a++;
            b++;
        }
        if (*b == '\0') {
            return true;
        }
    }
    return false;
}

bool hasFontExtension(const char *name)
{
    return containsIgnoreCase(name, ".ttc") || containsIgnoreCase(name, ".ttf") ||
           containsIgnoreCase(name, ".otf");
}

// 中文字体的优先级: 数字越小越优先, 负数 = 不是中文字体。
//
// 光看"含不含汉字"不够 —— 目录遍历顺序是按文件名的, DroidSansFallback
// 排在 Noto 前面就会被选中。板上首选文泉驿(镜像自带), 开发机次选 Noto。
int cjkFontRank(const char *name)
{
    static const struct {
        const char *key;
        int rank;
    } kKeys[] = {
        {"wqy-zenhei", 0},         // 文泉驿正黑 —— 板上装的就是这个
        {"wqy-microhei", 1},       // 文泉驿微米黑
        {"notosanscjk", 2},        // Noto Sans CJK
        {"notosanssc", 2},         // Noto Sans SC 子集
        {"sourcehansans", 3},      // 思源黑体
        {"droidsansfallback", 4},  // 安卓的兜底字体, 能用但不是首选
        {"uming", 5},              // AR PL UMing
    };
    for (const auto &k : kKeys) {
        if (containsIgnoreCase(name, k.key)) {
            return k.rank;
        }
    }
    return -1;
}

// 递归扫一个目录, 在 out 里保留优先级最高的那个 (best_rank 跨目录累积)。
// 深度限死 4 层: 字体目录不会更深, 也避免软链接成环。
bool scanDirForFont(const char *dir, int depth, char *out, size_t out_size, int *best_rank)
{
    if (depth > 4) {
        return false;
    }
    DIR *d = opendir(dir);
    if (d == nullptr) {
        return false;
    }
    bool found = false;
    for (struct dirent *e = readdir(d); e != nullptr; e = readdir(d)) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) {
            continue;
        }
        char path[1024];
        const int n = std::snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (n < 0 || static_cast<size_t>(n) >= sizeof path) {
            continue;
        }
        struct stat st {};
        if (lstat(path, &st) != 0) {  // lstat: 软链接不当目录走, 免得绕圈
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (scanDirForFont(path, depth + 1, out, out_size, best_rank)) {
                found = true;
            }
            continue;
        }
        if (!S_ISREG(st.st_mode) || !hasFontExtension(e->d_name)) {
            continue;
        }
        const int rank = cjkFontRank(e->d_name);
        if (rank >= 0 && (*best_rank < 0 || rank < *best_rank)) {
            std::snprintf(out, out_size, "%s", path);
            *best_rank = rank;
            found = true;
        }
    }
    closedir(d);
    return found;
}

// 候选表没命中时, 在常见字体目录里现场找 —— 发行版的包会把同一个字体装到
// 不同目录 (Fedora 现在是 /usr/share/fonts/google-noto-sans-cjk-vf-fonts/)。
const char *scanForCjkFont()
{
    static char best[1024];
    static int best_rank = -1;
    static const char *kRoots[] = {
        "/usr/share/fonts",
        "/usr/local/share/fonts",
    };
    for (const char *root : kRoots) {
        if (best_rank == 0) {
            break;  // 文泉驿已经是首选, 不用再找了
        }
        scanDirForFont(root, 0, best, sizeof best, &best_rank);
    }
    const char *home = std::getenv("HOME");
    if (best_rank != 0 && home != nullptr && *home != '\0') {
        char dir[1024];
        std::snprintf(dir, sizeof dir, "%s/.local/share/fonts", home);
        scanDirForFont(dir, 0, best, sizeof best, &best_rank);
        std::snprintf(dir, sizeof dir, "%s/.fonts", home);
        scanDirForFont(dir, 0, best, sizeof best, &best_rank);
    }
    return best_rank >= 0 ? best : nullptr;
}

const char *pickFontFile(const char *override_path)
{
    if (override_path != nullptr && *override_path != '\0') {
        if (fileReadable(override_path)) {
            return override_path;
        }
        std::fprintf(stderr, "警告: 指定的字体打不开, 改用自动查找: %s\n", override_path);
    }
    for (const char *path : kFontCandidates) {
        if (fileReadable(path)) {
            return path;
        }
    }
    if (const char *scanned = scanForCjkFont(); scanned != nullptr) {
        return scanned;
    }
    std::fprintf(stderr,
                 "警告: 找不到中文字体, 界面上的汉字会显示成方框。\n"
                 "      板上装一个: sudo apt install fonts-wqy-zenhei\n"
                 "      或指定文件: ./deffire-gui-dev --font=/path/to/font.ttc\n");
    return nullptr;
}

lv_font_t *createFont(const char *file, uint32_t px, const char *what)
{
    lv_font_t *font = lv_freetype_font_create(file,
                                              LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                              px, LV_FREETYPE_FONT_STYLE_NORMAL);
    if (font == nullptr) {
        std::fprintf(stderr, "警告: 加载字体失败 (%s): %s\n", what, file);
        return nullptr;
    }
    std::printf("  字体(%-8s): %s  [%u px]\n", what, file, px);
    return font;
}

// ---------------------------------------------------------------------------
// 按钮清单
//
// "有哪些按钮"是数据不是代码 —— 加按钮只改下面这张表, 布局按数量自动算。
// ---------------------------------------------------------------------------

// 按钮绑定的动作。**不要用标签文字来判断点了哪个按钮** —— 那样改一次文案就
// 会静默改掉功能, 而且编译器不会报错。加动作时在这里加一项, 然后在
// onAction() 的 switch 里补一个 case (漏了会有 -Wswitch 警告, 因为没写 default)。
enum class Action {
    // Nothing **必须留在第一位**: ButtonSpec 是聚合类型, 表里漏写 action 字段时,
    // action 会被值初始化为 0 (= 第一个枚举值)。第一位是 Nothing, 漏写就退化成
    // "占位", 顶多点了没反应; 若第一位是 LedOn, 漏写就会静默点亮 LED。
    Nothing,
    LedOn,
    LedOff,
    LedHeartbeat,
    BuzzerOn,
    BuzzerOff,
    BuzzerHeartbeat,
    OutBuzzerOn,   // 外部无源蜂鸣器 (GPIOA:6): 只置标志, 脉冲由主循环发
    OutBuzzerOff,
    OpenSensor,                   // 首页 → 信息源 N (N 在 ButtonSpec::arg)
    SettingsPage,                 // 设置页 (本地测试入口)
    LedSingleSettingPage,         // 设置 → LED 调试页
    BuzzerSingleSettingPage,      // 设置 → 蜂鸣器调试页
    OutBuzzerSingleSettingPage,   // 设置 → 外部无源蜂鸣器调试页
    BackToHome,                   // 任意子页 → 首页
};

struct ButtonSpec {
    const char *label;  // UTF-8 显示文字
    bool active;        // true = 已实现; false = 占位 (按钮上多画一圈 outline)
    Action action;      // 点击后执行的动作; 只有 active=true 时才有意义
    int arg = 0;        // 动作参数; 目前只给 Action::OpenSensor 用 (0..11)
};

// 顶栏右侧的【退出】。它有自己的回调 (onClickQuit), action 不参与派发, 所以填
// Nothing —— 但字段一样要写全 (见上面 Nothing 的说明)。
constexpr ButtonSpec kQuitButton = {"退出", true, Action::Nothing};

// 顶栏右侧的小【设置】。本地测试入口都在设置页里, 首页留给 12 个信息源。
constexpr ButtonSpec kSettingsButton = {"设置", true, Action::SettingsPage};

// 信息源页的【应用】: 打开这一路选好的串口 (设备 + 波特率)。
// 信息源接口页 (覆盖层) 的顶栏/设置按钮。
constexpr ButtonSpec kOverlayHome = {"首页", true, Action::BackToHome};
constexpr ButtonSpec kOverlayQuit = {"退出应用", true, Action::Nothing};
constexpr ButtonSpec kOverlayApply = {"应用设置", true, Action::Nothing};
// 原来预留的"占位"按钮: 串口是运行时可插拔的, 需要一个"重新枚举"的入口。
constexpr ButtonSpec kOverlayRefreshSerial = {"刷新串口", true, Action::Nothing};

// 一个页面 = 一组底栏按钮 + 页名 + **本页每行放几个**。
//
// "每行几个"是**每页各自**的参数, 不是全局常量: 首页排 6 列(2 行 × 6 列),
// 设置/调试页排 4 列, 信息源页 1 列且单按钮居中。
//
// 一行里每个按钮等宽(flex_grow), 所以在 1024 宽的屏上: 6 列 ≈ 159 px、
// 4 列 ≈ 243 px、3 列 ≈ 328 px —— 中文标签都不会被挤到换行;
// 触摸目标建议 ≥ 48 px 见方, 高度由 btn_h 保证。
struct PageSpec {
    const char *name;  // 页名: 打印到控制台, 也显示在内容区
    const ButtonSpec *buttons;
    int count;
    int columns;  // 本页底栏每行放几个按钮
};

// 首页: 12 个信息源 (传感器模块) 的入口。
//
// **只预留位置, 不填内容** —— 具体接几个探测器、每个模块有哪些数据项现在都不知道,
// 所以这里只有"信息源 1..12", 不预置"烟感/温度"这类猜出来的名字。
// 点进去的模块页显示"尚未配置数据项"; 等模块定义/数据接上后再往那一页填。
constexpr ButtonSpec kHomeButtons[] = {
    {"信息源 1", true, Action::OpenSensor, 0},
    {"信息源 2", true, Action::OpenSensor, 1},
    {"信息源 3", true, Action::OpenSensor, 2},
    {"信息源 4", true, Action::OpenSensor, 3},
    {"信息源 5", true, Action::OpenSensor, 4},
    {"信息源 6", true, Action::OpenSensor, 5},
    {"信息源 7", true, Action::OpenSensor, 6},
    {"信息源 8", true, Action::OpenSensor, 7},
    {"信息源 9", true, Action::OpenSensor, 8},
    {"信息源 10", true, Action::OpenSensor, 9},
    {"信息源 11", true, Action::OpenSensor, 10},
    {"信息源 12", true, Action::OpenSensor, 11},
};

// 设置页: 现在所有的本地测试入口。原来放在首页的三个硬件调试页挪到这里;
// 首页因此腾出位置给 12 个信息源。
constexpr ButtonSpec kSettingsButtons[] = {
    {"LED 调试", true, Action::LedSingleSettingPage},
    {"蜂鸣器 调试", true, Action::BuzzerSingleSettingPage},
    {"外部无源蜂鸣器", true, Action::OutBuzzerSingleSettingPage},
    {"返回", true, Action::BackToHome},
};

// 信息源模块页: 底栏只放【返回】。数据项在内容区那块面板里, 目前是空的。
constexpr ButtonSpec kSensorButtons[] = {
    {"返回", true, Action::BackToHome},
};

// LED 调试页: 单独操作 LED 的开关与心跳, 末位是返回
constexpr ButtonSpec kLedButtons[] = {
    {"LED 开", true, Action::LedOn},
    {"LED 关", true, Action::LedOff},
    {"LED 心跳", true, Action::LedHeartbeat},
    {"返回", true, Action::BackToHome},
};

// 蜂鸣器调试页: 同上, 换成蜂鸣器
constexpr ButtonSpec kBuzzerButtons[] = {
    {"蜂鸣器开", true, Action::BuzzerOn},
    {"蜂鸣器关", true, Action::BuzzerOff},
    {"蜂鸣器心跳", true, Action::BuzzerHeartbeat},
    {"返回", true, Action::BackToHome},
};

// 外部无源蜂鸣器调试页: 接在 GPIOA:6 上的无源蜂鸣器。
//
// 和上面那一页的区别: 走 libgpiod 手动写电平, **不归内核 leds-gpio 管**,
// 所以它没有 trigger 文件 —— 这页里**没有"心跳"按钮**, 板载那个的"心跳"是
// 内核 LED 框架的功能, 外部这个没有对应的东西 (开/关就是全部动作)。
constexpr ButtonSpec kOutBuzzerButtons[] = {
    {"外部蜂鸣器 开", true, Action::OutBuzzerOn},
    {"外部蜂鸣器 关", true, Action::OutBuzzerOff},
    {"返回", true, Action::BackToHome},
};

// 数组长度在编译期取出来 —— 原来 kNumButtons 是手写的数字, 往表里加一项忘了改
// 就变成越界初始化 (这次就是这么坏的)。
template <typename T, int N>
constexpr int countOf(const T (&)[N])
{
    return N;
}

// 当前显示哪一页。顺序必须与 kPages 一致, 见下面的 static_assert。
// Sensor 是**一类**页: 具体是 12 个里的哪一个由 g_sensor_index 决定。
enum class Page { Home, Settings, Sensor, Led, Buzzer, OutBuzzer };

constexpr PageSpec kPages[] = {
    {"首页", kHomeButtons, countOf(kHomeButtons), 6},  // 12 个信息源: 2 行 × 6 列
    {"设置", kSettingsButtons, countOf(kSettingsButtons), 4},
    {"信息源", kSensorButtons, countOf(kSensorButtons), 1},  // 单按钮, 居中
    {"LED 调试页", kLedButtons, countOf(kLedButtons), 4},
    {"蜂鸣器调试页", kBuzzerButtons, countOf(kBuzzerButtons), 4},
    {"外部无源蜂鸣器调试页", kOutBuzzerButtons, countOf(kOutBuzzerButtons), 3},
};

static_assert(static_cast<int>(Page::Home) == 0 &&
                  static_cast<int>(Page::Settings) == 1 &&
                  static_cast<int>(Page::Sensor) == 2 &&
                  static_cast<int>(Page::Led) == 3 &&
                  static_cast<int>(Page::Buzzer) == 4 &&
                  static_cast<int>(Page::OutBuzzer) == 5,
              "Page 的顺序必须与 kPages 一致");

static_assert(countOf(kHomeButtons) == kSensorCount,
              "首页按钮数量必须等于 kSensorCount (12 个信息源)");

// columns 会当除数用, 0 会让 rowsOf() 直接除零 —— 在编译期挡住, 别等运行期崩
constexpr bool columnsAllValid()
{
    for (const PageSpec &p : kPages) {
        if (p.columns < 1) {
            return false;
        }
    }
    return true;
}

static_assert(columnsAllValid(), "kPages 里每页的 columns 必须 >= 1");

// 某页要排几行; 以及所有页里最多的行数。
// 底栏高度按"最多的那页"算, 切页时底栏高度不变, 内容区不会上下跳。
constexpr int rowsOf(int count, int columns)
{
    return (count + columns - 1) / columns;
}

constexpr int maxRows()
{
    int m = 1;
    for (const PageSpec &p : kPages) {
        const int r = rowsOf(p.count, p.columns);
        if (r > m) {
            m = r;
        }
    }
    return m;
}

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------
lv_display_t *g_disp = nullptr;
lv_font_t *g_font_big = nullptr;
lv_font_t *g_font_small = nullptr;
lv_font_t *g_font_status = nullptr;

lv_obj_t *g_footer_bar = nullptr;  // 底栏容器: 只建一次, 切页时清空重建里面的按钮
lv_obj_t *g_page_label = nullptr;  // 内容区那行状态说明 (页名 + 上一步结果)
lv_obj_t *g_json_label = nullptr;  // 内容区那行 JSON 状态 (data / smoke)
unsigned g_json_revision = 0;      // 已经显示过的 JsonStatus::revision

// 内容区有两套内容: 普通页 (halloworld/JSON) 和 信息源模块页 (GPIO 选项 + 数据 + 状态)。
// 切页时只显示其中一套, 不销毁重建 —— 模块页的下拉框/状态要保留。
lv_obj_t *g_generic_content = nullptr;
lv_obj_t *g_sensor_page = nullptr;
lv_obj_t *g_sensor_title = nullptr;
lv_obj_t *g_sensor_device_dd = nullptr;  // 串口设备下拉
lv_obj_t *g_sensor_baud_dd = nullptr;    // 波特率下拉
lv_obj_t *g_sensor_status = nullptr;

// 数据项: 顺序 烟雾 / 火焰 / 温度 / 蜂鸣器 / LED, 每行一个"值"标签 + 一个"状态"标签。
// 值来自 json_status_for(当前信息源); 没有对应字段的行显示占位, 不显示假数据。
lv_obj_t *g_sensor_values[5] = {};
lv_obj_t *g_sensor_states[5] = {};

// 最近一次收到的完整 JSON 对象原文, 显示在信息源页「收到的报文」。
lv_obj_t *g_sensor_frame_label = nullptr;
unsigned g_sensor_frame_revision = 0;
int g_sensor_frame_source = -1;

// 接口页左侧的 12 个信息源按钮; 用来高亮当前选中的模块。
lv_obj_t *g_sensor_side_buttons[kSensorCount] = {};

// 每个信息源各自保存的串口选择 (设备路径 + 波特率)。真正打开/断线重连由
// serial_source.cpp 的后台线程负责; 这里只记 UI 的选择, 用来回填下拉框。
// 0 = 正常、1 = 异常由传感器模块那边定义, app 这边不做极性换算。
struct SensorConfig {
    std::string device;  // 空 = 未选择
    int baud = 0;        // 0 = 未选择
};
SensorConfig g_sensor_cfg[kSensorCount];

Page g_page = Page::Home;
int g_sensor_index = 0;  // Page::Sensor 时对应 0..11

// 切页请求。事件回调里**只置这个标志**, 真正的重建放到主循环里做 ——
// 否则会在"正在派发某个按钮的点击"的过程中删掉它所在的底栏。
bool g_page_dirty = false;

// 当前是不是 --windowed 预览模式 (全屏请求只在板上模式发, 见 ensureFullscreen)
bool g_windowed_mode = false;

// 上一步动作的结果, 显示在内容区 (nullptr = 刚切过页, 只显示页名)
const char *g_last_outcome = nullptr;

// 布局尺寸在 buildUi() 里算好存下来 —— 切页重建底栏时还要用 (见 buildPageButtons)
int g_win_w = kBoardWidth;
int g_btn_h = 48;
int g_margin = 10;

uint32_t g_start_tick = 0;
// g_quit 的定义在 main.cpp (程序级全局状态); 这里通过 start.h 的 extern 使用。

// LVGL 是否已经 lv_init() 过 —— gui_teardown() 要能安全处理"没建起来"的情况
// (例如 --help 提前返回), 不能对没初始化过的 LVGL 调 lv_deinit()。
bool g_lv_inited = false;

// 当前页名。Page::Sensor 是一类页, 名字要带上具体的信息源编号。
const char *pageName()
{
    if (g_page == Page::Sensor) {
        static char name[32];
        std::snprintf(name, sizeof name, "信息源 %d", g_sensor_index + 1);
        return name;
    }
    return kPages[static_cast<int>(g_page)].name;
}

// 内容区那行小字: "页名", 或 "页名 · 上一步结果"
void refreshPageLabel()
{
    if (g_page_label == nullptr) {
        return;
    }
    const char *name = pageName();
    if (g_last_outcome != nullptr) {
        lv_label_set_text_fmt(g_page_label, "%s · %s", name, g_last_outcome);
    } else {
        lv_label_set_text(g_page_label, name);
    }
}

// ---------------------------------------------------------------------------
// 串口选择 (设备 / 波特率)
//
// 设备清单在运行时枚举 (插了哪个口就列哪个), 波特率是标准值清单; 两者都以
// "未选择"打头, **没有默认值** —— 不猜模块接在哪个口、跑多少波特率。选完点
// 【应用设置】才真正去打开, 打开结果由 serial_source.cpp 记状态。
// ---------------------------------------------------------------------------

// 下拉里能选的设备 = 运行时枚举到的 /dev/tty*, 再加上**当前已配置但没枚举到**的
// 那个 (例如 FIRECONTROL_SERIAL_<N> 指定的 /dev/pts/… 或自定义路径)。
// 不把已配置的设备从下拉里弄丢 —— 否则一按【应用设置】就会被覆盖成"未选择"。
std::vector<std::string> deviceList(const std::string &configured)
{
    std::vector<std::string> devices = firecontrol::serial_list_devices();
    if (!configured.empty() &&
        std::find(devices.begin(), devices.end(), configured) == devices.end()) {
        devices.push_back(configured);
    }
    return devices;
}

// 设备下拉的选项文本 (LVGL dropdown 要 \n 分隔): "未选择\n/dev/ttyS0\n..."
std::string buildDeviceOptions(const std::string &configured)
{
    std::string options = "未选择";
    for (const std::string &device : deviceList(configured)) {
        options += '\n';
        options += device;
    }
    return options;
}

std::string buildBaudOptions()
{
    std::string options = "未选择";
    for (const int baud : firecontrol::serial_baud_rates()) {
        options += '\n';
        options += std::to_string(baud);
    }
    return options;
}

// 下拉框下标 <-> 实际值。0 一律表示"未选择"。
int deviceIndexOf(const std::string &device)
{
    if (device.empty()) {
        return 0;
    }
    const std::vector<std::string> devices = deviceList(device);
    for (std::size_t i = 0; i < devices.size(); i++) {
        if (devices[i] == device) {
            return static_cast<int>(i) + 1;
        }
    }
    return 0;
}

std::string deviceAt(int selected, const std::string &configured)
{
    if (selected <= 0) {
        return {};
    }
    const std::vector<std::string> devices = deviceList(configured);
    const std::size_t index = static_cast<std::size_t>(selected - 1);
    return index < devices.size() ? devices[index] : std::string();
}

int baudIndexOf(int baud)
{
    const std::vector<int> &rates = firecontrol::serial_baud_rates();
    for (std::size_t i = 0; i < rates.size(); i++) {
        if (rates[i] == baud) {
            return static_cast<int>(i) + 1;
        }
    }
    return 0;
}

int baudAt(int selected)
{
    if (selected <= 0) {
        return 0;
    }
    const std::vector<int> &rates = firecontrol::serial_baud_rates();
    const std::size_t index = static_cast<std::size_t>(selected - 1);
    return index < rates.size() ? rates[index] : 0;
}

// 从 serial_source 的当前配置回填 12 路的选择 (FIRECONTROL_SERIAL_<N> 在
// device_init 里已经先配过一次)。没配置的保持"未选择", 不猜设备/波特率。
void initSensorDefaults()
{
    for (int i = 0; i < kSensorCount; i++) {
        const firecontrol::SerialState state = firecontrol::serial_state(i);
        SensorConfig &cfg = g_sensor_cfg[i];
        cfg.device = state.configured ? state.device : std::string();
        cfg.baud = state.configured ? state.baud : 0;
    }
}

// 模块状态: 说清楚串口现在到底开没开 —— 不假装数据已经进来。
void updateSensorStatus()
{
    if (g_sensor_status == nullptr) {
        return;
    }

    const firecontrol::SerialState state = firecontrol::serial_state(g_sensor_index);
    if (!state.configured) {
        lv_label_set_text(g_sensor_status, "未选择串口（选好设备/波特率后点【应用设置】）");
        lv_obj_set_style_text_color(g_sensor_status, lv_color_hex(kSensorDim), 0);
        return;
    }

    if (state.opened) {
        lv_label_set_text_fmt(g_sensor_status, "%s @ %d（已打开，等待数据）",
                              state.device.c_str(), state.baud);
        lv_obj_set_style_text_color(g_sensor_status, lv_color_hex(kSensorOk), 0);
        return;
    }

    // 没打开: 原因原样带出来 (打开失败 / 设备已断开), 并说明会自动重连。
    lv_label_set_text_fmt(g_sensor_status, "%s @ %d 未打开：%s（每秒重连）",
                          state.device.c_str(), state.baud,
                          state.error.empty() ? "未知原因" : state.error.c_str());
    lv_obj_set_style_text_color(g_sensor_status, lv_color_hex(kSensorAccent), 0);
}

// 每帧看一眼串口状态, 只在 serial_state().revision 变了才重设标签 ——
// 否则每帧 set_text 会让 LVGL 一直重排布局。
void refreshSensorStatus()
{
    static unsigned last_revision = ~0u;
    static int last_source = -1;

    const firecontrol::SerialState state = firecontrol::serial_state(g_sensor_index);
    if (last_source == g_sensor_index && last_revision == state.revision) {
        return;
    }
    last_source = g_sensor_index;
    last_revision = state.revision;
    updateSensorStatus();
}

// 下拉框改了但还没点【应用】: 明确告诉用户"有未应用的更改"。
void markSensorDirty()
{
    if (g_sensor_status != nullptr) {
        lv_label_set_text(g_sensor_status, "有未应用的更改");
        lv_obj_set_style_text_color(g_sensor_status, lv_color_hex(kSensorAccent), 0);
    }
}

void refreshSensorFrameLabel(bool force);
void refreshSensorData(bool force);

// 左侧信息源按钮的选中态: 当前模块描边加粗、底色提亮。
void refreshSidebarSelection()
{
    for (int i = 0; i < kSensorCount; i++) {
        lv_obj_t *btn = g_sensor_side_buttons[i];
        if (btn == nullptr) {
            continue;
        }
        const bool selected = (i == g_sensor_index);
        lv_obj_set_style_border_width(btn, selected ? 2 : 1, 0);
        lv_obj_set_style_border_color(btn,
                                      lv_color_hex(selected ? kSensorAccent : kSensorLine), 0);
        lv_obj_set_style_bg_color(btn,
                                  lv_color_hex(selected ? kSensorSelected : kSensorPanel), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        if (selected) {
            lv_obj_scroll_to_view(btn, LV_ANIM_OFF);
        }
    }
}

// 把当前模块保存的选择填回控件。切页 / 首次进入模块页时调用。
// 没选过就停在"未选择"; 不把下拉框里显示的第一个值当成已保存。
void loadSensorControls()
{
    if (g_sensor_device_dd == nullptr || g_sensor_baud_dd == nullptr) {
        return;
    }

    const SensorConfig &cfg = g_sensor_cfg[g_sensor_index];
    if (g_sensor_title != nullptr) {
        lv_label_set_text_fmt(g_sensor_title, "信息源 %d", g_sensor_index + 1);
    }

    // 设备清单可能变过 (插拔), 每次进页都重建一遍再回填选择。
    const std::string options = buildDeviceOptions(cfg.device);
    lv_dropdown_set_options(g_sensor_device_dd, options.c_str());
    lv_dropdown_set_selected(g_sensor_device_dd, deviceIndexOf(cfg.device));
    lv_dropdown_set_selected(g_sensor_baud_dd, baudIndexOf(cfg.baud));

    updateSensorStatus();
    refreshSidebarSelection();
    refreshSensorFrameLabel(true);
    refreshSensorData(true);
}

// 【刷新串口】: 重新枚举 /dev/tty*, 尽量保住当前选择。
void onRefreshSerialList(lv_event_t *e)
{
    LV_UNUSED(e);
    if (g_sensor_device_dd == nullptr) {
        return;
    }

    const std::string current =
        deviceAt(lv_dropdown_get_selected(g_sensor_device_dd),
                 g_sensor_cfg[g_sensor_index].device);
    const std::string options = buildDeviceOptions(current);
    lv_dropdown_set_options(g_sensor_device_dd, options.c_str());
    lv_dropdown_set_selected(g_sensor_device_dd, deviceIndexOf(current));

    std::printf("串口列表已刷新 (%d 个设备)\n",
                static_cast<int>(firecontrol::serial_list_devices().size()));
    std::fflush(stdout);
}

void onSensorOptionChanged(lv_event_t *e)
{
    LV_UNUSED(e);
    markSensorDirty();
}

// 【应用设置】: 真正去打开这一路的串口。设备/波特率有一个没选 = 取消配置
// (关闭端口)。打开结果由 serial_source 记状态, 这里只显示。
void onApplySensorConfig(lv_event_t *e)
{
    LV_UNUSED(e);

    SensorConfig &cfg = g_sensor_cfg[g_sensor_index];
    cfg.device = deviceAt(lv_dropdown_get_selected(g_sensor_device_dd), cfg.device);
    cfg.baud = baudAt(lv_dropdown_get_selected(g_sensor_baud_dd));

    const bool configured = !cfg.device.empty() && cfg.baud > 0;
    const int rc = firecontrol::serial_configure(
        g_sensor_index, configured ? cfg.device.c_str() : nullptr,
        configured ? cfg.baud : 0);

    if (configured) {
        std::printf("信息源 %d 应用串口: %s @ %d (%s)\n", g_sensor_index + 1,
                    cfg.device.c_str(), cfg.baud, rc == 0 ? "已打开" : "打开失败");
    } else {
        std::printf("信息源 %d 应用串口: 未选择（已取消配置）\n", g_sensor_index + 1);
    }
    std::fflush(stdout);

    updateSensorStatus();
}

// 内容区切换: 普通页显示 halloworld/JSON, 信息源页显示 GPIO 选项/数据/状态。
void refreshPageContent()
{
    if (g_generic_content == nullptr || g_sensor_page == nullptr) {
        return;
    }

    if (g_page == Page::Sensor) {
        lv_obj_add_flag(g_generic_content, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(g_sensor_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_sensor_page);  // 盖住普通页的顶栏/底栏
        loadSensorControls();
    } else {
        lv_obj_remove_flag(g_generic_content, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_sensor_page, LV_OBJ_FLAG_HIDDEN);
    }
}

// 内容区那行 JSON 状态。
//
// 只在 JsonStatus::revision 变化时重设标签: status() 每 200 ms 刷新一次,
// 文件内容没变时 revision 不动, 这里不会每帧都触发 LVGL 重新布局。
// 解析失败也显示出来 —— "未识别"本身就是要让人看到的状态。
void refreshJsonLabel()
{
    if (g_json_label == nullptr) {
        return;
    }

    const firecontrol::JsonStatus json = firecontrol::json_status();
    if (g_json_revision == json.revision && g_json_revision != 0) {
        return;
    }
    g_json_revision = json.revision;

    if (json.valid) {
        // 只把这一帧里真的有的字段列出来 (模块可能只发其中几个)。
        std::string text = "JSON  ";
        if (json.has_temperature) {
            char temperature[32] = {};
            std::snprintf(temperature, sizeof temperature, "%.1f", json.temperature);
            text += "温度=";
            text += temperature;
            text += "  ";
        }
        if (json.has_fire_alarm) {
            text += "火焰=" + std::to_string(json.fire_alarm) + "  ";
        }
        if (json.has_smoke_alarm) {
            text += "烟雾=" + std::to_string(json.smoke_alarm) + "  ";
        }
        if (json.has_buzzer_state) {
            text += "蜂鸣=" + std::to_string(json.buzzer_state) + "  ";
        }
        if (json.has_led_state) {
            text += "LED=" + std::to_string(json.led_state) + "  ";
        }
        if (json.has_data) {
            text += "data=" + std::to_string(json.data) + "  ";
        }
        if (json.has_smoke) {
            text += "smoke=" + std::to_string(json.smoke) + "  ";
        }
        lv_label_set_text(g_json_label, text.c_str());
        lv_obj_set_style_text_color(g_json_label, lv_color_hex(kColText), 0);
    } else {
        lv_label_set_text_fmt(g_json_label, "JSON  未识别 (%s)",
                              json.error != nullptr ? json.error : "未知错误");
        // 解析失败用强调色, 别让"未识别"混在次要文字里被忽略。
        lv_obj_set_style_text_color(g_json_label, lv_color_hex(kColAccent), 0);
    }
}

// 信息源页底部的"收到的报文": 每个信息源各自一份完整帧。
// 只有收到完整 JSON 对象才更新; 还没接线的信息源显示"暂无"。
void refreshSensorFrameLabel(bool force = false)
{
    if (g_sensor_frame_label == nullptr) {
        return;
    }

    const firecontrol::JsonStatus json = firecontrol::json_status_for(g_sensor_index);
    if (!force && g_sensor_frame_source == g_sensor_index &&
        g_sensor_frame_revision == json.revision && g_sensor_frame_revision != 0) {
        return;
    }
    g_sensor_frame_source = g_sensor_index;
    g_sensor_frame_revision = json.revision;

    if (json.has_frame && !json.frame.empty()) {
        lv_label_set_text(g_sensor_frame_label, json.frame.c_str());
        lv_obj_set_style_text_color(g_sensor_frame_label, lv_color_hex(kSensorValue), 0);
    } else {
        lv_label_set_text(g_sensor_frame_label, "暂无");
        lv_obj_set_style_text_color(g_sensor_frame_label, lv_color_hex(kSensorDim), 0);
    }
}

// 一行数据: 值 + 状态。present=false 时值显示 "--" (灰)。
void setSensorRow(int row, bool present, const std::string &value,
                  const std::string &state, uint32_t state_color)
{
    if (row < 0 || row >= 5) {
        return;
    }
    if (g_sensor_values[row] != nullptr) {
        lv_label_set_text(g_sensor_values[row], present ? value.c_str() : "--");
        lv_obj_set_style_text_color(g_sensor_values[row],
                                    lv_color_hex(present ? kSensorValue : kSensorDim), 0);
    }
    if (g_sensor_states[row] != nullptr) {
        lv_label_set_text(g_sensor_states[row], state.c_str());
        lv_obj_set_style_text_color(g_sensor_states[row], lv_color_hex(state_color), 0);
    }
}

// 把当前信息源的解析结果填进接收卡五行。
//
// 字段对应 (模块只发这几个):
//   烟雾状态   ← smoker_alarm
//   火焰状态   ← fire_alarm
//   温度状态   ← temperature
//   蜂鸣器状态 ← buzzer_alarm
//   LED状态    ← payload 里没有对应字段, 规则也还没定 → 保持占位
//
// 0 = 正常 / 1 = 异常 由模块定义, app 只按 0/非0 显示, 不做极性换算。
// 缺字段的行显示"待接入"占位 —— 不拿上一帧的旧值冒充当前值。
void refreshSensorData(bool force = false)
{
    if (g_sensor_values[0] == nullptr) {
        return;
    }

    static unsigned last_revision = 0;
    static int last_source = -1;

    const firecontrol::JsonStatus json = firecontrol::json_status_for(g_sensor_index);
    if (!force && last_source == g_sensor_index && last_revision == json.revision) {
        return;
    }
    last_source = g_sensor_index;
    last_revision = json.revision;

    // 烟雾/火焰: 0=无、1=有 (模块定义), app 只显示。
    auto yesNoText = [](int value, const char *no, const char *yes) {
        return value == 0 ? no : yes;
    };
    auto yesNoColor = [](int value) { return value == 0 ? kSensorOk : kSensorAccent; };

    if (json.has_smoke_alarm) {
        setSensorRow(0, true, std::to_string(json.smoke_alarm),
                     yesNoText(json.smoke_alarm, "无烟", "有烟"),
                     yesNoColor(json.smoke_alarm));
    } else {
        setSensorRow(0, false, "", "待接入", kSensorDim);
    }

    if (json.has_fire_alarm) {
        setSensorRow(1, true, std::to_string(json.fire_alarm),
                     yesNoText(json.fire_alarm, "无火", "有火"),
                     yesNoColor(json.fire_alarm));
    } else {
        setSensorRow(1, false, "", "待接入", kSensorDim);
    }

    if (json.has_temperature) {
        char temperature[32] = {};
        std::snprintf(temperature, sizeof temperature, "%.1f", json.temperature);
        // 阈值还没定: 给数值 + 明确写"阈值待定", 不擅自判正常/异常。
        setSensorRow(2, true, temperature, "阈值待定", kSensorAccent);
    } else {
        setSensorRow(2, false, "", "待接入", kSensorDim);
    }

    // 蜂鸣器状态机: 0=灭 1=急促(100ms) 2=缓慢(500ms) 3=长响。
    static const char *kBuzzerNames[] = {"灭", "急促", "缓慢", "长响"};
    if (json.has_buzzer_state) {
        const int value = json.buzzer_state;
        const bool known = (value >= 0 && value <= 3);
        setSensorRow(3, true, std::to_string(value), known ? kBuzzerNames[value] : "未知",
                     value == 0 ? kSensorDim : kSensorAccent);
    } else {
        setSensorRow(3, false, "", "待接入", kSensorDim);
    }

    // 三色 LED 状态机: 0=灭, 1=蓝闪(烟), 2=绿闪(温), 3=红闪(火),
    // 4=蓝绿(烟+温), 5=蓝红(烟+火), 6=绿红(温+火), 7=红绿蓝轮转(三路全报)。
    static const char *kLedNames[] = {"灭",  "蓝闪", "绿闪", "红闪",
                                      "蓝绿", "蓝红", "绿红", "轮转"};
    if (json.has_led_state) {
        const int value = json.led_state;
        const bool known = (value >= 0 && value <= 7);
        setSensorRow(4, true, std::to_string(value), known ? kLedNames[value] : "未知",
                     value == 0 ? kSensorDim : kSensorAccent);
    } else {
        setSensorRow(4, false, "", "待接入", kSensorDim);
    }
}

bool inStartupGrace()
{
    const uint32_t elapsed = lv_tick_get() - g_start_tick;
    if (elapsed >= kIgnoreClicksMs) {
        return false;
    }
    std::printf("忽略启动瞬间的点击 (启动后 %u ms)\n", elapsed);
    std::fflush(stdout);
    return true;
}

// ---------------------------------------------------------------------------
// 事件回调
// ---------------------------------------------------------------------------
void onClickQuit(lv_event_t *e)
{
    LV_UNUSED(e);
    if (inStartupGrace()) {
        return;
    }
    std::printf("点击【退出】按钮, 退出\n");
    std::fflush(stdout);
    g_quit = true;
}

void onAction(lv_event_t *e)
{
    if (inStartupGrace()) {
        return;
    }
    const auto *spec = static_cast<const ButtonSpec *>(lv_event_get_user_data(e));

    // 打印坐标便于板上核对触摸是否偏移 (板上待办之一)
    lv_point_t p{0, 0};
    lv_indev_t *indev = lv_indev_active();
    if (indev != nullptr) {
        lv_indev_get_point(indev, &p);
    }
    const int px = static_cast<int>(p.x);
    const int py = static_cast<int>(p.y);

    // 结果先算出来再统一打印 + 显示到内容区 —— 控制台的格式保持不变
    // (板上核对触摸/硬件的记录都依赖 "点击【x】坐标 (x,y) → 结果" 这一行)。
    //
    // 初值给个兜底串而不是 nullptr: 下面 switch 覆盖了所有枚举值, 但枚举的底层
    // 整数理论上可以是表外的值, GCC 因此无法证明它非空 (-Wformat-overflow)。
    // 用兜底串既消掉警告, 也真的兜住了那种情况 —— 而不是靠加 default 把
    // -Wswitch 那道"漏了 case"的防线拆掉。
    const char *outcome = "(未知动作)";
    char outcome_buf[64] = {};  // OpenSensor 的 "进入信息源 N" 用
    bool page_switch = false;

    // 按动作派发。switch 里不写 default —— 以后往 Action 里加成员但忘了加
    // case 时, -Wswitch 会在编译期报出来, 而不是让那个按钮默默什么都不做。
    switch (spec->action) {
    case Action::LedOn:
        outcome = led_onboard_on() == 0 ? "LED 已点亮" : "失败 (开发机无此设备)";
        break;
    case Action::LedOff:
        outcome = led_onboard_off() == 0 ? "LED 已熄灭" : "失败 (开发机无此设备)";
        break;
    case Action::LedHeartbeat:
        outcome = led_onboard_set_heartbeat() == 0 ? "LED 心跳已开启"
                                                   : "失败 (开发机无此设备)";
        break;
    case Action::BuzzerOn:
        outcome = buzzer_onboard_on() == 0 ? "蜂鸣器已响" : "失败 (开发机无此设备)";
        break;
    case Action::BuzzerOff:
        outcome = buzzer_onboard_off() == 0 ? "蜂鸣器已停" : "失败 (开发机无此设备)";
        break;
    case Action::BuzzerHeartbeat:
        outcome = buzzer_onboard_set_heartbeat() == 0 ? "蜂鸣器心跳已开启"
                                                      : "失败 (开发机无此设备)";
        break;
    // 外部无源蜂鸣器: 这里只是**开/关请求** —— 真正的方波由一个专用线程按固定
    // 频率发 (见 start.cpp 的 buzzer_out_thread)。不能在这里"响一下"就算完:
    // 无源蜂鸣器要的是持续方波, 单个脉冲听不见东西。
    case Action::OutBuzzerOn:
        buzzer_out_set(true);
        outcome = "外部无源蜂鸣器已开";
        break;
    case Action::OutBuzzerOff:
        buzzer_out_set(false);
        outcome = "外部无源蜂鸣器已停";
        break;
    case Action::Nothing:
        outcome = "(占位, 功能待定)";
        break;

    // 首页的 12 个信息源入口: 具体是哪一个由 ButtonSpec::arg 决定。
    // Page::Sensor 是同一类页, 用 g_sensor_index 区分。
    case Action::OpenSensor: {
        int idx = spec->arg;
        if (idx < 0 || idx >= kSensorCount) {
            idx = 0;  // 表外参数不该出现; 退到第 1 个, 不越界
        }
        g_sensor_index = idx;
        g_page = Page::Sensor;
        g_page_dirty = true;
        page_switch = true;
        std::snprintf(outcome_buf, sizeof outcome_buf, "进入信息源 %d", idx + 1);
        outcome = outcome_buf;
        break;
    }
    // 顶栏小【设置】: 本地测试入口都收在这一页。
    case Action::SettingsPage:
        g_page = Page::Settings;
        g_page_dirty = true;
        page_switch = true;
        outcome = "进入设置";
        break;

    // 切页: 只改 g_page 并置标志, 底栏由主循环重建 (见 g_page_dirty 的说明)
    case Action::LedSingleSettingPage:
        g_page = Page::Led;
        g_page_dirty = true;
        page_switch = true;
        outcome = "进入 LED 调试页";
        break;
    case Action::BuzzerSingleSettingPage:
        g_page = Page::Buzzer;
        g_page_dirty = true;
        page_switch = true;
        outcome = "进入蜂鸣器调试页";
        break;
    case Action::OutBuzzerSingleSettingPage:
        g_page = Page::OutBuzzer;
        g_page_dirty = true;
        page_switch = true;
        outcome = "进入外部无源蜂鸣器调试页";
        break;
    case Action::BackToHome:
        g_page = Page::Home;
        g_page_dirty = true;
        page_switch = true;
        outcome = "返回首页";
        break;
    }

    std::printf("点击【%s】坐标 (%d,%d) → %s\n", spec->label, px, py, outcome);

    // 切页时页名自己会变, 就不把"进入 xx 页"当成结果留在屏幕上; 设备动作则
    // 把结果显示出来 —— 板上不用连控制台也能看到刚才那一下成没成。
    if (page_switch) {
        g_last_outcome = nullptr;
    } else {
        g_last_outcome = outcome;
        refreshPageLabel();
    }
    std::fflush(stdout);
}

void onKey(lv_event_t *e)
{
    const uint32_t key = lv_event_get_key(e);
    if (key == 'q' || key == 'Q' || key == LV_KEY_ESC) {
        std::printf("按键退出 (q / Esc)\n");
        std::fflush(stdout);
        g_quit = true;
    }
}

// ---------------------------------------------------------------------------
// 请窗口管理器把我们的窗口设为全屏 (EWMH _NET_WM_STATE_FULLSCREEN)
//
// 为什么需要: 板上是 Openbox + lxpanel。应用建的是 1024x600 (= 整屏) 的窗口,
// Openbox 再往上加标题栏, 窗口总高就超过屏幕, 底部被推到屏幕外; lxpanel 又占着
// 屏幕最下面一条 —— 结果就是底栏按钮只剩上半截。
// 按 workflow.md §2.2 的决定, 全屏交给 DE: 应用只**请求**, 铺不铺满由 WM 决定,
// 不手工算坐标、也不自己造无边框窗口。
//
// LVGL 的 X11 驱动不暴露窗口句柄/全屏接口, 所以这里自己开一条 X 连接, 按标题
// 找到刚建好的窗口, 往 root 发一个 ClientMessage。找不到/没 WM 时静默失败。
// ---------------------------------------------------------------------------
// 按标题找窗口。注意 **WM 的框架窗口和 client 窗口同名** (Openbox/mutter 都这样),
// 而 _NET_WM_STATE 必须发给 client —— 所以先递归子窗口 (深的优先), 自己没有子窗口
// 命中时才用自己。按"最内层同名窗口"取, 才不会把请求发给框架。
Window findWindowByTitle(Display *d, Window window, const char *title)
{
    Window root = None;
    Window parent = None;
    Window *children = nullptr;
    unsigned int count = 0;
    if (XQueryTree(d, window, &root, &parent, &children, &count) != 0) {
        for (unsigned int i = 0; i < count; i++) {
            const Window found = findWindowByTitle(d, children[i], title);
            if (found != None) {
                XFree(children);
                return found;
            }
        }
        XFree(children);
    }

    char *name = nullptr;
    if (XFetchName(d, window, &name) && name != nullptr) {
        const bool hit = std::strcmp(name, title) == 0;
        XFree(name);
        if (hit) {
            return window;
        }
    }
    return None;
}

bool requestFullscreen();  // 定义在后面

// 启动后重试几次: 窗口要等 LVGL 自己 flush 了映射请求、WM 处理完才会出现,
// 所以请求放在主循环里发 (render()), 而不是 gui_build() 里。
void ensureFullscreen()
{
    static int attempts = 0;
    static uint32_t next_tick = 0;

    if (g_windowed_mode || attempts >= 8) {
        return;
    }
    const uint32_t now = lv_tick_get();
    if (attempts > 0 && now < next_tick) {
        return;
    }
    attempts++;
    next_tick = now + 250;
    requestFullscreen();
}

bool requestFullscreen()
{
    Display *d = XOpenDisplay(nullptr);
    if (d == nullptr) {
        return false;
    }

    const Window win = findWindowByTitle(d, DefaultRootWindow(d), kTitle);
    if (win == None) {
        XCloseDisplay(d);
        return false;
    }

    // 已经是全屏就什么都不用做 (重试时会走到这里)。
    const Atom wm_state_atom = XInternAtom(d, "_NET_WM_STATE", False);
    const Atom fullscreen_atom = XInternAtom(d, "_NET_WM_STATE_FULLSCREEN", False);
    {
        Atom type = None;
        int format = 0;
        unsigned long count = 0;
        unsigned long after = 0;
        unsigned char *data = nullptr;
        if (XGetWindowProperty(d, win, wm_state_atom, 0, 32, False, XA_ATOM, &type,
                               &format, &count, &after, &data) == Success &&
            data != nullptr) {
            bool is_fullscreen = false;
            for (unsigned long i = 0; i < count; i++) {
                if (reinterpret_cast<Atom *>(data)[i] == fullscreen_atom) {
                    is_fullscreen = true;
                }
            }
            XFree(data);
            if (is_fullscreen) {
                XCloseDisplay(d);
                return true;
            }
        }
    }

    XEvent event {};
    event.xclient.type = ClientMessage;
    event.xclient.window = win;
    event.xclient.message_type = wm_state_atom;
    event.xclient.format = 32;
    event.xclient.data.l[0] = 1;  // _NET_WM_STATE_ADD
    event.xclient.data.l[1] = static_cast<long>(fullscreen_atom);
    event.xclient.data.l[2] = 0;
    event.xclient.data.l[3] = 1;  // 普通应用发起的请求

    XSendEvent(d, DefaultRootWindow(d), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(d);
    XCloseDisplay(d);
    return true;
}

// 显示对象被删掉时会被调用 —— 无论是我们自己删, 还是窗口管理器点了关闭。
void onDisplayDeleted(lv_event_t *e)
{
    LV_UNUSED(e);
    g_disp = nullptr;  // 退出时别重复删
    g_quit = true;
}

// ---------------------------------------------------------------------------
// 界面
//
// 三段式工业面板: 顶部标题栏 (品牌 + 状态灯 + 退出) / 中间内容区 / 底部操作栏。
// 颜色一律取自上表的调色板, 控件代码里不出现颜色字面量;
// 布局用 flex, 换屏幕尺寸或改按钮数量都不用改坐标。
// ---------------------------------------------------------------------------

// 纯布局用的"素容器": 透明、无描边、不滚动、无内边距
lv_obj_t *createPane(lv_obj_t *parent)
{
    lv_obj_t *pane = lv_obj_create(parent);
    lv_obj_remove_flag(pane, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(pane, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pane, 0, 0);
    lv_obj_set_style_radius(pane, 0, 0);
    lv_obj_set_style_pad_all(pane, 0, 0);
    lv_obj_set_style_shadow_width(pane, 0, 0);  // 主题给容器带了卡片样式, 阴影一律去掉
    return pane;
}

// 顶栏/底栏的公共外观: 深色底 + 靠内侧一条 1px 分隔线
lv_obj_t *createBar(lv_obj_t *parent, int w, int h, lv_border_side_t side)
{
    lv_obj_t *bar = createPane(parent);
    lv_obj_set_size(bar, w, h);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColPanel), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, side, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(kColSeparator), 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return bar;
}

void applyButtonSkin(lv_obj_t *btn, bool active)
{
    lv_obj_set_style_radius(btn, 4, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);

    // 已实现的按钮: 有实体面 + 常规描边; 未实现的压暗成"底", 一眼能看出还没做
    lv_obj_set_style_bg_color(btn, lv_color_hex(active ? kColSurface : kColBg), 0);
    lv_obj_set_style_border_color(btn,
                                  lv_color_hex(active ? kColSeparator : kColDividerDim), 0);

    // 触摸反馈: 底色抬一档 + 描边点亮成强调色 —— 手指按下去要立刻有回应
    lv_obj_set_style_bg_color(btn, lv_color_hex(kColPressed), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(kColAccent), LV_STATE_PRESSED);
}

lv_obj_t *createButton(lv_obj_t *parent, const ButtonSpec &spec, lv_event_cb_t on_click)
{
    lv_obj_t *btn = lv_button_create(parent);
    applyButtonSkin(btn, spec.active);
    // 回调里要拿整个 spec (标签 + 动作), 借 user_data 把表里那一项的地址传过去。
    // kButtons 是 constexpr 静态存储, 生命周期覆盖整个程序, 指针一直有效。
    lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED,
                        const_cast<ButtonSpec *>(&spec));

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, spec.label);
    // 颜色显式设在标签上: 主题对 label 类有自己的一套配色, 靠父对象继承不保险
    lv_obj_set_style_text_color(label,
                                lv_color_hex(spec.active ? kColText : kColTextDim), 0);
    if (g_font_small != nullptr) {
        lv_obj_set_style_text_font(label, g_font_small, 0);
    }
    lv_obj_center(label);
    return btn;
}

// 顶部标题栏: 左 = 品牌(强调色竖条 + 名字), 中 = 状态灯, 右 = 退出
void buildHeader(lv_obj_t *scr, int win_w, int bar_h, int margin)
{
    lv_obj_t *bar = createBar(scr, win_w, bar_h, LV_BORDER_SIDE_BOTTOM);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_hor(bar, margin, 0);

    // --- 左: 品牌 ---
    lv_obj_t *brand = createPane(bar);
    lv_obj_set_size(brand, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(brand, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brand, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(brand, 10, 0);

    lv_obj_t *tick = createPane(brand);
    lv_obj_set_size(tick, 3, 18);
    lv_obj_set_style_bg_color(tick, lv_color_hex(kColAccent), 0);
    lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(brand);
    lv_label_set_text(title, kTitle);
    lv_obj_set_style_text_color(title, lv_color_hex(kColText), 0);
    lv_obj_set_style_text_letter_space(title, 1, 0);
    if (g_font_small != nullptr) {
        lv_obj_set_style_text_font(title, g_font_small, 0);
    }

    // --- 中: 状态灯 ---
    // 现在只表示"程序在跑"。以后接上传感器/联动状态时, 改这里的文字和灯色即可。
    lv_obj_t *state = createPane(bar);
    lv_obj_set_size(state, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(state, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(state, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(state, 8, 0);

    lv_obj_t *led = createPane(state);
    lv_obj_set_size(led, 10, 10);
    lv_obj_set_style_radius(led, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(led, lv_color_hex(kColOk), 0);
    lv_obj_set_style_bg_opa(led, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(led, 8, 0);  // 一点辉光, 让灯真的"亮"起来
    lv_obj_set_style_shadow_color(led, lv_color_hex(kColOk), 0);
    lv_obj_set_style_shadow_opa(led, LV_OPA_50, 0);

    lv_obj_t *state_text = lv_label_create(state);
    lv_label_set_text(state_text, "运行中");
    lv_obj_set_style_text_color(state_text, lv_color_hex(kColTextDim), 0);
    if (g_font_status != nullptr) {
        lv_obj_set_style_text_font(state_text, g_font_status, 0);
    }

    // --- 右: 设置 + 退出 ---
    // 用一个右侧小容器把两个按钮放在一起, 避免 SPACE_BETWEEN 把设置按钮
    // 拉到顶栏中间。容器宽度显式算出来: LV_SIZE_CONTENT 在"先建容器、后设
    // 子对象尺寸"的顺序下可能还没算进子对象, 设置按钮会看不见。
    const int top_btn_h = bar_h - 26;
    const int settings_w = pct(win_w, 7);
    const int quit_w = pct(win_w, 9);
    constexpr int kTopButtonGap = 8;

    lv_obj_t *right = createPane(bar);
    lv_obj_set_size(right, settings_w + kTopButtonGap + quit_w, top_btn_h);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *settings_btn = createButton(right, kSettingsButton, onAction);
    lv_obj_set_size(settings_btn, settings_w, top_btn_h);

    lv_obj_t *quit_btn = createButton(right, kQuitButton, onClickQuit);
    lv_obj_set_size(quit_btn, quit_w, top_btn_h);
}

// 中间内容区。两套内容共用这块区域:
//   普通页: halloworld + 页名 + JSON 状态
//   信息源页: GPIO 选项 / 数据项 / 模块状态 (剩下的留空)
void buildContent(lv_obj_t *scr, int win_w, int top, int h)
{
    lv_obj_t *content = createPane(scr);
    lv_obj_set_size(content, win_w, h);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, top);

    // ---------------- 普通页 ----------------
    lv_obj_t *generic = createPane(content);
    lv_obj_set_size(generic, win_w, h);
    lv_obj_align(generic, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_flex_flow(generic, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(generic, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(generic, 14, 0);
    g_generic_content = generic;

    lv_obj_t *title = lv_label_create(generic);
    lv_label_set_text(title, kText);
    lv_obj_set_style_text_color(title, lv_color_hex(kColText), 0);
    lv_obj_set_style_text_letter_space(title, 3, 0);
    if (g_font_big != nullptr) {
        lv_obj_set_style_text_font(title, g_font_big, 0);
    }

    // 装饰用的强调色短线: 不承载信息, 只是把视线钉在标题上
    lv_obj_t *rule = createPane(generic);
    lv_obj_set_size(rule, 72, 3);
    lv_obj_set_style_bg_color(rule, lv_color_hex(kColAccent), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);

    // 页名 / 上一步结果。切页和每次点按钮之后由 refreshPageLabel() 更新 ——
    // 板上没接控制台时, 这一行就是"刚才那一下成没成"的唯一反馈。
    lv_obj_t *status = lv_label_create(generic);
    lv_obj_set_style_text_color(status, lv_color_hex(kColTextDim), 0);
    if (g_font_status != nullptr) {
        lv_obj_set_style_text_font(status, g_font_status, 0);
    }
    lv_label_set_text(status, pageName());
    g_page_label = status;

    // JSON 状态: 最基础的 {"data":1,"smoke":1} 解析结果。
    lv_obj_t *json = lv_label_create(generic);
    lv_obj_set_style_text_color(json, lv_color_hex(kColTextDim), 0);
    if (g_font_status != nullptr) {
        lv_obj_set_style_text_font(json, g_font_status, 0);
    }
    lv_label_set_text(json, "JSON  等待数据");
    g_json_label = json;
    refreshJsonLabel();

    // 信息源接口页现在是一个独立的覆盖层, 由 buildSensorOverlay() 创建。
}

// 底部操作栏的容器: 深色面板 + 顶边分隔线。
//
// 容器只建一次, 里面的按钮由 buildPageButtons() 按当前页重建 —— 切页就是
// "清空底栏, 按新页的按钮表重排"。高度按 maxRows() (所有页里最多的行数) 算,
// 切页时底栏高度不变, 内容区不会上下跳。
//
// 外层竖直 flex、每行横向 flex 均分 —— 加按钮只改页表, 布局自动重排,
// "每行几个"取自本页的 PageSpec::columns。返回底栏高度, 供 buildUi 算内容区。
int buildFooter(lv_obj_t *scr, int win_w, int btn_h, int margin)
{
    const int row_gap = btn_h / 3;  // 行间距, 随按钮高度缩放
    const int bar_h = maxRows() * btn_h + (maxRows() - 1) * row_gap + 2 * margin;

    lv_obj_t *bar = createBar(scr, win_w, bar_h, LV_BORDER_SIDE_TOP);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(bar, margin, 0);
    lv_obj_set_style_pad_row(bar, row_gap, 0);
    // createBar 默认是横向 flex, 这里要竖直排行
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_COLUMN);
    // 主轴(main place)用 CENTER, 不是 SPACE_BETWEEN: 首页 2 行时正好排满(无差别),
    // 而只有 1 行的调试页会在空出来的高度里**垂直居中** —— 按钮不会全挤在分隔线
    // 下面、底下拖一条空带。代价: 切到调试页时那一行比首页第 1 行低约半行。
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    g_footer_bar = bar;
    return bar_h;
}

// 按当前页 (g_page) 重建底栏按钮: 初始化和每次切页都走这里。
// 尺寸不用重算 —— 底栏高度已由 buildFooter 按 maxRows() 固定;
// 本页每行放几个由 PageSpec::columns 决定。
void buildPageButtons()
{
    if (g_footer_bar == nullptr) {
        return;
    }
    lv_obj_clean(g_footer_bar);  // 删掉上一页的按钮

    const PageSpec &page = kPages[static_cast<int>(g_page)];
    const int grid_w = g_win_w - 2 * g_margin;
    const int rows = rowsOf(page.count, page.columns);
    // 只有一个按钮的页 (信息源模块页的【返回】) 居中显示, 不拉成整行宽。
    const bool single = (page.count == 1);

    for (int r = 0; r < rows; r++) {
        lv_obj_t *row = createPane(g_footer_bar);
        lv_obj_set_size(row, grid_w, g_btn_h);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row,
                              single ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, g_margin, 0);

        // 本行的槽位固定是 columns 个: 末行不满时补**透明占位**, 让已有的按钮
        // 保持等宽。不补的话 flex_grow 会把末行 2 个按钮撑成半屏宽, 和上一行
        // 对不齐(就是待办里"两行宽度不等"那个老问题)。
        const int first = r * page.columns;
        for (int i = first; i < first + page.columns; i++) {
            if (i < page.count) {
                lv_obj_t *btn = createButton(row, page.buttons[i], onAction);
                lv_obj_set_height(btn, g_btn_h);
                if (single) {
                    lv_obj_set_width(btn, pct(g_win_w, 25));
                } else {
                    lv_obj_set_flex_grow(btn, 1);
                }
            } else {
                lv_obj_t *slot = createPane(row);  // 透明占位: 只占宽度, 不显示
                lv_obj_set_height(slot, g_btn_h);
                lv_obj_set_flex_grow(slot, 1);
            }
        }
    }
    refreshPageLabel();
}

// ---------------------------------------------------------------------------
// 信息源接口页 (覆盖层)
//
// 参考样图: 深蓝灰底 + 琥珀描边; 顶栏(品牌/首页/退出) + 左侧信息源列表 +
// 中间接收卡片 + 右侧信息源设置。整页是 screen 的最后一个子对象, 只在
// Page::Sensor 时显示, 会盖住普通页的顶栏/底栏。
// ---------------------------------------------------------------------------
void applySensorButtonSkin(lv_obj_t *btn, bool active, bool selected)
{
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_bg_color(btn,
                              lv_color_hex(selected ? kSensorSelected : kSensorPanel), 0);
    lv_obj_set_style_bg_opa(btn, active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, selected ? 2 : 1, 0);
    lv_obj_set_style_border_color(btn,
                                  lv_color_hex(selected ? kSensorAccent
                                                        : (active ? kSensorLine : kSensorDim)), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kSensorSelected), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(kSensorAccent), LV_STATE_PRESSED);
}

lv_obj_t *createSensorLabel(lv_obj_t *parent, const char *text, uint32_t color,
                            const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    if (font != nullptr) {
        lv_obj_set_style_text_font(label, font, 0);
    }
    return label;
}

lv_obj_t *createSensorButton(lv_obj_t *parent, const ButtonSpec &spec,
                             lv_event_cb_t cb, bool selected = false)
{
    lv_obj_t *btn = lv_button_create(parent);
    applySensorButtonSkin(btn, spec.active, selected);

    if (cb != nullptr) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED,
                            const_cast<ButtonSpec *>(&spec));
    }

    const uint32_t label_color = selected ? kSensorAccent
                                           : (spec.active ? kSensorText : kSensorDim);
    lv_obj_t *label = createSensorLabel(btn, spec.label, label_color, g_font_small);
    lv_obj_center(label);
    return btn;
}

void styleSensorDropdownList(lv_obj_t *list)
{
    if (list == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(list, lv_color_hex(kSensorPanel), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(list, 1, 0);
    lv_obj_set_style_border_color(list, lv_color_hex(kSensorLine), 0);
    lv_obj_set_style_text_color(list, lv_color_hex(kSensorValue), 0);
    if (g_font_small != nullptr) {
        lv_obj_set_style_text_font(list, g_font_small, 0);
    }
    lv_obj_set_style_bg_color(list, lv_color_hex(kSensorAccent), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_text_color(list, lv_color_hex(kSensorBg), LV_PART_SELECTED);
}

void onSensorDropdownReady(lv_event_t *e)
{
    styleSensorDropdownList(lv_dropdown_get_list(lv_event_get_target_obj(e)));
}

void styleSensorDropdown(lv_obj_t *dd)
{
    lv_obj_set_style_bg_color(dd, lv_color_hex(kSensorPanel), 0);
    lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dd, 1, 0);
    lv_obj_set_style_border_color(dd, lv_color_hex(kSensorLine), 0);
    lv_obj_set_style_radius(dd, 8, 0);
    lv_obj_set_style_text_color(dd, lv_color_hex(kSensorValue), 0);
    lv_obj_set_style_pad_all(dd, 10, 0);
    if (g_font_small != nullptr) {
        lv_obj_set_style_text_font(dd, g_font_small, 0);
    }
    lv_obj_set_style_border_color(dd, lv_color_hex(kSensorAccent), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(dd, 0, LV_STATE_FOCUSED);
    lv_dropdown_set_symbol(dd, nullptr);
    lv_dropdown_set_selected_highlight(dd, false);
    lv_obj_add_event_cb(dd, onSensorDropdownReady, LV_EVENT_READY, nullptr);
    styleSensorDropdownList(lv_dropdown_get_list(dd));
}

lv_obj_t *createSensorDropdownRow(lv_obj_t *parent, lv_obj_t **out_dd,
                                  const char *options, lv_event_cb_t cb)
{
    lv_obj_t *row = createPane(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 44);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);

    lv_obj_t *dd = lv_dropdown_create(row);
    lv_dropdown_set_options(dd, options);
    lv_obj_set_height(dd, 44);
    lv_obj_set_flex_grow(dd, 1);
    styleSensorDropdown(dd);
    if (cb != nullptr) {
        lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    }
    createSensorLabel(row, "v", kSensorDim, g_font_status);

    if (out_dd != nullptr) {
        *out_dd = dd;
    }
    return row;
}

lv_obj_t *createSensorCard(lv_obj_t *parent, int height)
{
    lv_obj_t *card = createPane(parent);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, height);
    lv_obj_set_style_bg_color(card, lv_color_hex(kSensorPanel), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(kSensorLine), 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_row(card, 4, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    return card;
}

// 一行数据: 名称 / 值 / 状态。返回值标签; 需要状态标签时传 out_state。
// 值和状态是两回事 (比如温度有值但"阈值待定"), 所以两个标签都要能改。
lv_obj_t *addSensorDataRow(lv_obj_t *parent, const char *name, const char *value,
                           const char *state, bool placeholder,
                           lv_obj_t **out_state = nullptr)
{
    lv_obj_t *row = createPane(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 24);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *name_label = createSensorLabel(row, name, kSensorDim, g_font_status);
    lv_obj_set_width(name_label, LV_PCT(45));

    lv_obj_t *value_label = createSensorLabel(row, value,
                                              placeholder ? kSensorDim : kSensorValue,
                                              g_font_small);
    lv_obj_set_width(value_label, LV_PCT(20));
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *state_label = createSensorLabel(row, state,
                                              placeholder ? kSensorDim : kSensorOk,
                                              g_font_status);
    lv_obj_set_width(state_label, LV_PCT(30));
    lv_obj_set_style_text_align(state_label, LV_TEXT_ALIGN_RIGHT, 0);
    if (out_state != nullptr) {
        *out_state = state_label;
    }
    return value_label;
}

void buildSensorTopBar(lv_obj_t *parent, int win_w, int top_h)
{
    lv_obj_t *bar = createPane(parent);
    lv_obj_set_size(bar, win_w, top_h);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kSensorBg), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(kSensorLine), 0);
    lv_obj_set_style_pad_hor(bar, 16, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *left = createPane(bar);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left, 10, 0);

    lv_obj_t *box = createPane(left);
    lv_obj_set_size(box, 18, 18);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(kSensorAccent), 0);
    lv_obj_set_style_radius(box, 2, 0);

    createSensorLabel(left, "火灾控制终端", kSensorText, g_font_small);
    createSensorLabel(left, "|", kSensorDim, g_font_status);

    lv_obj_t *dot = createPane(left);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(kColOk), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    createSensorLabel(left, "运行中", kSensorOk, g_font_status);

    lv_obj_t *home = createSensorButton(bar, kOverlayHome, onAction);
    lv_obj_set_size(home, 220, 42);

    lv_obj_t *quit = createSensorButton(bar, kOverlayQuit, onClickQuit);
    lv_obj_set_size(quit, 160, 42);
}

void buildSensorSidebar(lv_obj_t *parent, int left_w, int top_h, int body_h)
{
    lv_obj_t *side = createPane(parent);
    lv_obj_set_size(side, left_w, body_h);
    lv_obj_align(side, LV_ALIGN_TOP_LEFT, 0, top_h);
    lv_obj_set_style_bg_color(side, lv_color_hex(kSensorBg), 0);
    lv_obj_set_style_bg_opa(side, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(side, 1, 0);
    lv_obj_set_style_border_side(side, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_color(side, lv_color_hex(kSensorLine), 0);
    lv_obj_set_style_pad_all(side, 12, 0);
    lv_obj_set_style_pad_row(side, 10, 0);
    lv_obj_set_flex_flow(side, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(side, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(side, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(side, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(side, LV_SCROLLBAR_MODE_AUTO);

    for (int i = 0; i < kSensorCount; i++) {
        lv_obj_t *btn = createSensorButton(side, kHomeButtons[i], onAction,
                                           i == g_sensor_index);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, 56);
        g_sensor_side_buttons[i] = btn;
    }
}

void buildSensorMain(lv_obj_t *parent, int main_w, int left_w, int top_h, int body_h)
{
    lv_obj_t *main = createPane(parent);
    lv_obj_set_size(main, main_w, body_h);
    lv_obj_align(main, LV_ALIGN_TOP_LEFT, left_w, top_h);
    lv_obj_set_style_bg_color(main, lv_color_hex(kSensorBg), 0);
    lv_obj_set_style_bg_opa(main, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(main, 14, 0);
    lv_obj_set_style_pad_row(main, 14, 0);
    lv_obj_set_flex_flow(main, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // 接收卡片: 五行先显示占位, 真正进入某一路后由 refreshSensorData() 填。
    lv_obj_t *recv = createSensorCard(main, 230);
    createSensorLabel(recv, "接收：", kSensorAccent, g_font_small);
    g_sensor_values[0] = addSensorDataRow(recv, "烟雾状态", "--", "待接入", true,
                                          &g_sensor_states[0]);
    g_sensor_values[1] = addSensorDataRow(recv, "火焰状态", "--", "待接入", true,
                                          &g_sensor_states[1]);
    g_sensor_values[2] = addSensorDataRow(recv, "温度状态", "--", "待接入", true,
                                          &g_sensor_states[2]);
    g_sensor_values[3] = addSensorDataRow(recv, "蜂鸣器状态", "--", "待接入", true,
                                          &g_sensor_states[3]);
    g_sensor_values[4] = addSensorDataRow(recv, "LED状态", "--", "规则待定", true,
                                          &g_sensor_states[4]);

    // 模块状态 / 完整报文卡片
    lv_obj_t *info = createSensorCard(main, 200);
    lv_obj_add_flag(info, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(info, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(info, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *status_row = createPane(info);
    lv_obj_set_width(status_row, LV_PCT(100));
    lv_obj_set_height(status_row, 24);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_row, 8, 0);
    createSensorLabel(status_row, "模块状态：", kSensorDim, g_font_status);
    g_sensor_status = createSensorLabel(status_row, "未配置", kSensorValue, g_font_status);

    createSensorLabel(info, "收到的报文：", kSensorDim, g_font_status);
    g_sensor_frame_label = createSensorLabel(info, "暂无", kSensorValue, g_font_status);
    lv_obj_set_width(g_sensor_frame_label, LV_PCT(100));
    lv_label_set_long_mode(g_sensor_frame_label, LV_LABEL_LONG_WRAP);

    lv_obj_t *reserve = createPane(info);
    lv_obj_set_width(reserve, LV_PCT(100));
    lv_obj_set_height(reserve, 24);
    lv_obj_set_flex_flow(reserve, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(reserve, 8, 0);
    createSensorLabel(reserve, "预留：", kSensorDim, g_font_status);
    createSensorLabel(reserve, "", kSensorDim, g_font_status);
}

void buildSensorSettings(lv_obj_t *parent, int right_w, int x, int top_h, int body_h)
{
    lv_obj_t *right = createPane(parent);
    lv_obj_set_size(right, right_w, body_h);
    lv_obj_align(right, LV_ALIGN_TOP_LEFT, x, top_h);
    lv_obj_set_style_bg_color(right, lv_color_hex(kSensorBg), 0);
    lv_obj_set_style_bg_opa(right, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(right, 1, 0);
    lv_obj_set_style_border_side(right, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(right, lv_color_hex(kSensorLine), 0);
    lv_obj_set_style_pad_all(right, 16, 0);
    lv_obj_set_style_pad_row(right, 12, 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    createSensorLabel(right, "信息源设置", kSensorAccent, g_font_small);

    const std::string device_options = buildDeviceOptions(g_sensor_cfg[g_sensor_index].device);
    createSensorLabel(right, "串口设备", kSensorDim, g_font_status);
    createSensorDropdownRow(right, &g_sensor_device_dd, device_options.c_str(),
                            onSensorOptionChanged);

    const std::string baud_options = buildBaudOptions();
    createSensorLabel(right, "波特率", kSensorDim, g_font_status);
    createSensorDropdownRow(right, &g_sensor_baud_dd, baud_options.c_str(),
                            onSensorOptionChanged);

    // 原来的"占位"按钮 —— 串口可以插拔, 给一个重新枚举的入口。
    lv_obj_t *refresh = createSensorButton(right, kOverlayRefreshSerial,
                                           onRefreshSerialList);
    lv_obj_set_width(refresh, LV_PCT(100));
    lv_obj_set_height(refresh, 44);

    // 把"应用设置"推到面板底部
    lv_obj_t *spacer = createPane(right);
    lv_obj_set_width(spacer, LV_PCT(100));
    lv_obj_set_flex_grow(spacer, 1);

    lv_obj_t *apply = createSensorButton(right, kOverlayApply, onApplySensorConfig, true);
    lv_obj_set_width(apply, LV_PCT(100));
    lv_obj_set_height(apply, 44);
}

void buildSensorOverlay(lv_obj_t *scr, int win_w, int win_h)
{
    lv_obj_t *overlay = createPane(scr);
    lv_obj_set_size(overlay, win_w, win_h);
    lv_obj_align(overlay, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(kSensorBg), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    g_sensor_page = overlay;

    const int top_h = pct(win_h, 10);
    const int left_w = pct(win_w, 22);
    const int right_w = pct(win_w, 24);
    const int body_h = win_h - top_h;

    buildSensorTopBar(overlay, win_w, top_h);
    buildSensorSidebar(overlay, left_w, top_h, body_h);
    buildSensorMain(overlay, win_w - left_w - right_w, left_w, top_h, body_h);
    buildSensorSettings(overlay, right_w, win_w - right_w, top_h, body_h);

    lv_obj_move_foreground(overlay);
}

void buildUi(int win_w, int win_h)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColBg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    g_win_w = win_w;
    g_margin = pct(win_w, 1);
    initSensorDefaults();

    // 底栏按钮高度。触摸屏上手指触点约 40~50 px, 原来取屏高 9%(1024x600 上 54 px)
    // —— 实测**手感偏紧, 不好按**, 所以提到 13%: 同一块屏上是 78 px(高了 44%)。
    // 上下限挡住极端窗口尺寸: 最小 48(再小就真的难按), 最大 96(再高会把内容区挤没)。
    g_btn_h = pct(win_h, 13);
    if (g_btn_h < 48) {
        g_btn_h = 48;
    }
    if (g_btn_h > 96) {
        g_btn_h = 96;
    }

    const int header_h = pct(win_h, 11);

    buildHeader(scr, win_w, header_h, g_margin);
    const int footer_h = buildFooter(scr, win_w, g_btn_h, g_margin);
    buildContent(scr, win_w, header_h, win_h - header_h - footer_h);
    buildSensorOverlay(scr, win_w, win_h);
    buildPageButtons();  // 首页
}

// 开发机窗口模式下给鼠标画个指针。板上是触摸屏, 而且 LVGL 的 X11 后端
// 会把 X 光标隐藏掉, 所以只在窗口模式加。
void addMouseCursor(void)
{
    lv_indev_t *indev = lv_indev_get_next(nullptr);
    while (indev != nullptr) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_obj_t *cur = lv_label_create(lv_layer_top());
            lv_label_set_text(cur, LV_SYMBOL_GPS);
            // 深底上用强调色, 别用主题默认色
            lv_obj_set_style_text_color(cur, lv_color_hex(kColAccent), 0);
            lv_indev_set_cursor(indev, cur);
            return;
        }
        indev = lv_indev_get_next(indev);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// gui_build(): 建界面 —— 只调一次。原来的 main() 前半段(探 X → lv_init → 字体 →
// 窗口 → 输入 → 布局)整段搬到这里。
//
// 命令行参数来自 main.cpp 定义的 g_argc / g_argv(四个阶段的签名保持不带参数)。
// 返回值: 0 成功; -1 失败(具体原因已经打到 stderr)。
// ---------------------------------------------------------------------------
int gui_build()
{
    bool windowed = false;
    const char *font_override = std::getenv("FIRECONTROL_FONT");

    for (int i = 1; i < g_argc; i++) {
        if (std::strcmp(g_argv[i], "--windowed") == 0) {
            windowed = true;
        } else if (std::strncmp(g_argv[i], "--font=", 7) == 0) {
            font_override = g_argv[i] + 7;
        } else if (std::strcmp(g_argv[i], "-h") == 0 ||
                   std::strcmp(g_argv[i], "--help") == 0) {
            std::printf("用法: %s [--windowed] [--font=<字体文件>]\n"
                        "  (无参数)     按屏幕尺寸建窗口 —— 板上用这个 (是否铺满由 WM 决定)\n"
                        "  --windowed   1024x600 窗口模式 —— 开发机预览用\n"
                        "  --font=FILE  指定字体文件 (默认按候选表自动找)\n"
                        "  -h, --help   显示本帮助\n"
                        "\n"
                        "交互: 点右上角【退出】按钮退出, 或按 q / Esc\n",
                        g_argv[0]);
            // 帮助打完就结束: 置退出标志, cpp_start 的循环一轮都不会跑。
            // (gui_teardown 对"没建过界面"是安全的, 见 g_lv_inited。)
            g_quit = true;
            return 0;
        }
    }

    // FreeType 按 UTF-8 处理文本, 同时保证 printf 出的中文不乱码
    if (std::setlocale(LC_ALL, "") == nullptr) {
        std::fprintf(stderr, "警告: setlocale 失败, 中文可能显示异常\n");
    }

    // -----------------------------------------------------------------------
    // 先探一下 X server
    //
    // 两件事: 无参数模式要知道屏幕实际尺寸 (LVGL 的 X11 后端只会按给它的尺寸
    // 建窗口, 不会自己查屏幕), 以及连不上时把排查方法讲清楚。
    // 这个连接只是查询, 查完就关 —— 真正绘图的连接由 LVGL 自己开。
    // -----------------------------------------------------------------------
    Display *probe = XOpenDisplay(nullptr);
    if (probe == nullptr) {
        std::fprintf(stderr,
                     "无法连接 X server。\n"
                     "  DISPLAY = %s\n"
                     "\n"
                     "板上排查:\n"
                     "  1) ls /tmp/.X11-unix/          确认 X server 在跑 (应看到 X0)\n"
                     "  2) export DISPLAY=:0           串口/ssh 会话里默认没有这个变量\n"
                     "  3) export XAUTHORITY=<xauth>   Xorg 带 -auth 时的认证文件\n",
                     std::getenv("DISPLAY") ? std::getenv("DISPLAY") : "(未设置)");
        return -1;
    }

    const int scr_n = DefaultScreen(probe);
    const int screen_w = DisplayWidth(probe, scr_n);
    const int screen_h = DisplayHeight(probe, scr_n);
    const int screen_depth = DefaultDepth(probe, scr_n);
    const char *vendor = ServerVendor(probe);
    std::printf("已连接 X server\n");
    std::printf("  服务端 : %s\n", vendor);
    std::printf("  屏幕   : %dx%d, 深度 %d\n", screen_w, screen_h, screen_depth);
    XCloseDisplay(probe);

    // 窗口尺寸按屏幕实际尺寸算。
    //
    // 板上与开发机都是 LXDE/X11 (板上 WM 为 Openbox), 所以这个窗口会被 WM 接管:
    // 开发机上实测窗口被加了装饰、报了 1074x687 而不是请求的 1024x600。
    // 按 2026-10-18 的决定, 全屏交给 DE —— 应用侧只需按 EWMH 发
    // _NET_WM_STATE_FULLSCREEN 请求 (或配 LXDE/Openbox 窗口规则), 不自己实现
    // (见 workflow.md §2.2 / WARNING.md C-7)。
    const int win_w = windowed ? kBoardWidth : screen_w;
    const int win_h = windowed ? kBoardHeight : screen_h;
    std::printf("  窗口   : %dx%d%s\n", win_w, win_h,
                windowed ? " (窗口模式)" : " (按屏幕尺寸)");

    // -----------------------------------------------------------------------
    // LVGL 初始化
    // -----------------------------------------------------------------------
    lv_init();
    g_lv_inited = true;

    // FreeType 已经由 lv_init() 初始化过了 (third_party/lvgl/src/lv_init.c:
    // lv_freetype_init(LV_FREETYPE_CACHE_FT_GLYPH_CNT))。**不要再调一次** ——
    // 第二次会因为 ft_ctx 已存在而返回 LV_RESULT_INVALID, 于是这里必然误报
    // "FreeType 初始化失败, 中文可能显示为方框", 而中文其实是正常的。板上日志
    // 里同样会出现那句, 会把上板验收引向错误方向。
    // 缓存大小要调就改 lv_conf.h 的 LV_FREETYPE_CACHE_FT_GLYPH_CNT; 字形到底
    // 能不能用, 由 createFont() 的失败分支负责报错。

    const char *font_file = pickFontFile(font_override);
    if (font_file != nullptr) {
        g_font_big = createFont(font_file, kBigFontPx, "主标题");
        g_font_small = createFont(font_file, kSmallFontPx, "按钮标签");
        g_font_status = createFont(font_file, kStatusFontPx, "状态文字");
    }

    g_disp = lv_x11_window_create(kTitle, win_w, win_h);
    if (g_disp == nullptr) {
        std::fprintf(stderr, "创建 X11 窗口失败\n");
        return -1;
    }
    lv_x11_inputs_create(g_disp, nullptr);
    lv_display_add_event_cb(g_disp, onDisplayDeleted, LV_EVENT_DELETE, nullptr);

    // 板上 (Openbox + lxpanel): 不请求全屏的话, 标题栏把窗口往下推、lxpanel 再盖住
    // 最下面一条, 底栏按钮会被切掉一半。真正的请求在主循环里发 (见 ensureFullscreen)
    // —— 这里窗口还没被 LVGL flush/映射, 现在发会被 WM 忽略。
    g_windowed_mode = windowed;
    if (!windowed) {
        std::printf("  全屏   : 请求 _NET_WM_STATE_FULLSCREEN（主循环里发，铺不铺满由 WM 决定）\n");
    }

    // 输入采样压得比刷新快: 刷新 16ms 一次决定"画面多久更新", 采样 5ms 一次决定
    // "多久看一眼有没有按下"。两者用同一个值会让快点击被整次吞掉(见 kIndevReadMs)。
    for (lv_indev_t *indev = lv_indev_get_next(nullptr); indev != nullptr;
         indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_timer_t *timer = lv_indev_get_read_timer(indev);
            if (timer != nullptr) {
                lv_timer_set_period(timer, kIndevReadMs);
            }
        }
    }

    // -----------------------------------------------------------------------
    // q / Esc 要能全局生效
    //
    // LVGL 的键盘输入只发给"组里当前被聚焦的对象", 组里没有聚焦对象时按键会
    // 被直接丢掉。所以把屏幕本身加进默认组并聚焦它, 让屏幕当快捷键的接收者。
    // (按钮不进组, 所以点按钮不会把焦点抢走 —— 见 indev_click_focus。)
    // -----------------------------------------------------------------------
    lv_group_t *group = lv_group_get_default();
    if (group != nullptr) {
        lv_group_add_obj(group, lv_screen_active());
        lv_group_focus_obj(lv_screen_active());
    }
    lv_obj_add_event_cb(lv_screen_active(), onKey, LV_EVENT_KEY, nullptr);

    buildUi(win_w, win_h);

    if (windowed) {
        addMouseCursor();
    }

    std::printf("  布局   : 深色工业风 —— 顶栏(品牌/状态灯/设置/退出) + 内容区 + 分页底栏"
                " (%d 页, 首页 %d 个信息源)\n",
                countOf(kPages), kPages[static_cast<int>(Page::Home)].count);
    std::printf("\n交互:\n");
    std::printf("  点右上角【退出】   退出程序\n");
    std::printf("  点右上角【设置】   进设置页 (LED / 蜂鸣器 / 外部无源蜂鸣器调试入口)\n");
    std::printf("  首页点【信息源 1..12】 进模块页, 页内【返回】回首页\n");
    std::printf("  设置页点【LED 调试】/【蜂鸣器 调试】/【外部无源蜂鸣器】 进对应调试页\n");
    std::printf("  调试页里点按钮     单独控制板载 LED / 蜂鸣器 / 外部无源蜂鸣器\n");
    std::printf("  按 q / Esc        退出程序 (每一页都一样)\n");
    std::fflush(stdout);

    g_start_tick = lv_tick_get();
    return 0;
}

// ---------------------------------------------------------------------------
// render(): 刷一帧 —— 主循环在 cpp_start() 里, 这里只做"一帧"该做的事。
// 返回值: 0(留给将来"这一帧出错要不要停"的语义, 现在不阻断循环)。
// ---------------------------------------------------------------------------
int render()
{
    // 切页在这里落地: 事件回调只置标志, 避免在派发点击的过程中删掉底栏
    // (见 g_page_dirty 的说明)。重建完刷新内容区那行小字。
    if (g_page_dirty) {
        g_page_dirty = false;
        buildPageButtons();
        refreshPageContent();
        std::printf("切换到 %s (底栏 %d 个按钮)\n", pageName(),
                    kPages[static_cast<int>(g_page)].count);
        std::fflush(stdout);
    }

    // 外部无源蜂鸣器的方波由它自己的线程发, 这里不用管 —— 主循环只管界面。
    //
    // X11 后端自己有两个定时器在读 X 事件 (显示 5 ms / 输入 1 ms), 这里只需要
    // 按 LVGL 给的间隔睡一下即可。
    ensureFullscreen();  // 窗口映射后再请 WM 全屏 (只发请求, 见 workflow §2.2)

    refreshJsonLabel();
    refreshSensorFrameLabel();
    refreshSensorStatus();  // 串口打开/断线/重连 (异步) —— 变了才重设
    refreshSensorData();    // 接收卡五行 (帧到达/字段变化时)

    uint32_t idle = lv_timer_handler();
    if (idle == LV_NO_TIMER_READY || idle > kMaxSleepMs) {
        idle = kMaxSleepMs;
    }
    usleep(idle * 1000);
    return 0;
}

// ---------------------------------------------------------------------------
// gui_teardown(): 收尾 —— 停蜂鸣器线程 / 关显示 / 删字体 / lv_deinit()。
// 对"界面没建起来"的情况也必须安全(例如 --help 或建窗口失败)。
// ---------------------------------------------------------------------------
void gui_teardown()
{
    std::printf("退出\n");

    // 先停蜂鸣器: 停声 + 把 PA6 还回去 + join 发声线程。要在销毁显示/字体之前,
    // 免得线程还在往 GPIO 写的时候进程被拆掉。
    buzzer_out_shutdown();

    if (g_disp != nullptr) {
        lv_display_delete(g_disp);  // 关 X 窗口, 停掉 tick 线程
        g_disp = nullptr;
    }
    if (g_font_big != nullptr) {
        lv_freetype_font_delete(g_font_big);
        g_font_big = nullptr;
    }
    if (g_font_small != nullptr) {
        lv_freetype_font_delete(g_font_small);
        g_font_small = nullptr;
    }
    if (g_font_status != nullptr) {
        lv_freetype_font_delete(g_font_status);
        g_font_status = nullptr;
    }
    if (g_lv_inited) {
        lv_deinit();
        g_lv_inited = false;
    }
}
