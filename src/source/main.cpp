#include "greeting.h"
#include "start.h"

// ---------------------------------------------------------------------------
// 程序级全局变量 —— 定义就在这里, 声明在 start.h, 其它文件用 extern 引用。
//
//  本项目支持C\C++混写
//
// 只放"整个程序共享"的状态。GUI 的内部句柄(g_disp / g_font_* / g_footer_bar /
// g_page / g_page_dirty / g_win_w …)留在 gui.cpp, 那是界面的实现细节。
//
// 把定义放这里而不是库里的好处: 打开 main.cpp 就能看到"程序有哪些全局状态";
// 代价是 firecontrol 静态库里对这些符号的引用要由可执行文件提供 —— 链接没问题
// (main.cpp 在每个产物里各编一份), 单元测试也不会拉 start.cpp.o。
// ---------------------------------------------------------------------------
bool g_quit = false;   // 主循环退出标志: 按钮 / 按键 / 关窗都会置它
int g_argc = 0;        // 命令行参数: 只有 gui_build() 用(--windowed / --font=)
char **g_argv = nullptr;

int main(int argc, char **argv)
{
    // 参数是"程序级状态", 先收进全局, 再由 gui_build() 解析 —— 四个阶段
    // (device_init/status/gui_build/render)的签名因此保持不带参数。
    g_argc = argc;
    g_argv = argv;

    // 真正的代码在 src/library/start.cpp 的 cpp_start() 里(含唯一的主循环)。
    // 这里只保留"入口"这一件事: 调用, 然后返回它的结果。
    return cpp_start();
}
