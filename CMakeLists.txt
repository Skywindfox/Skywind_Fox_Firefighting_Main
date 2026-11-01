cmake_minimum_required(VERSION 3.20)

project(FireControlApp
    VERSION 1.0.0
    DESCRIPTION "FireControlApp"
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# 全局编译选项
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

add_compile_options(-Wall -Wextra)

# 导出 compile_commands.json, 供 VS Code 的 C/C++ 扩展精确解析每个文件的
# 编译参数(标准、宏、头文件路径)。这比手写 includePath 靠谱得多。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# 是否交叉编译
#
# CMAKE_CROSSCOMPILING 只在用了 toolchain file 时为真。
# ---------------------------------------------------------------------------
set(IS_CROSS_BUILD OFF)
if(CMAKE_CROSSCOMPILING)
    set(IS_CROSS_BUILD ON)
elseif(CMAKE_CXX_COMPILER_TARGET MATCHES "arm|aarch64|mips|riscv")
    set(IS_CROSS_BUILD ON)
endif()

# ---------------------------------------------------------------------------
# 版本号 = 本仓库的 commit 次数；产物名 FireFv0.<次数>
#
# 目的: 每次提交编出来的文件名不同, 板上/桌面来回拷不会互相覆盖, 也不会把上一版
# 当成这一版。GUI 产物 FireFv0.<n>, 控制台版 FireFv0.<n>-console。
#
# 交叉编译走容器时 .git 不进镜像 (.dockerignore 排除了), 所以 build-armhf.sh /
# build-armhf.ps1 会用 -DFIRE_VERSION_COUNT=... 把次数传进来; 原生构建直接数 git。
# 实际名字同时写进 <build>/FIRE_VERSION.txt, 部署脚本从那里读。
# ---------------------------------------------------------------------------
set(FIRE_VERSION_COUNT "" CACHE STRING "commit 次数; 留空则用 git rev-list --count HEAD")
if(FIRE_VERSION_COUNT STREQUAL "")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-list --count HEAD
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE _fire_git_count
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
    endif()
    if(NOT _fire_git_count MATCHES "^[0-9]+$")
        set(_fire_git_count 0)
    endif()
    set(FIRE_VERSION_COUNT "${_fire_git_count}")
endif()

set(FIRE_VERSION "0.${FIRE_VERSION_COUNT}")
set(FIRE_APP_NAME "FireFv${FIRE_VERSION}")
set(FIRE_CONSOLE_NAME "${FIRE_APP_NAME}-console")

message(STATUS "版本: ${FIRE_APP_NAME} (第 ${FIRE_VERSION_COUNT} 个提交)")
file(WRITE "${CMAKE_BINARY_DIR}/FIRE_VERSION.txt"
     "version=${FIRE_VERSION}\ncommit_count=${FIRE_VERSION_COUNT}\napp=${FIRE_APP_NAME}\nconsole=${FIRE_CONSOLE_NAME}\n")

# ---------------------------------------------------------------------------
# cJSON (third_party) —— 最基础的 JSON 解析
#
# 源码随仓库提交 (third_party/cjson, 1.7.18, MIT), 不依赖系统 libcjson:
#   - 开发机 Fedora 只有运行库, 没有头文件;
#   - 板上 Debian 12 也得另装 libcjson-dev（否则没有头文件）;
#   - 交叉编译还要在 armhf 镜像里再装一份, 版本容易漂。
# 只有两个文件, 直接编进静态库最省事, 构建结果也可复现。
#
# 注意: cJSON 是 C 代码, 所以顶层 project() 要同时启用 C 和 CXX。
# 第三方代码不参与本项目"零警告"的要求, 编译选项里加 -w。
# ---------------------------------------------------------------------------
add_library(cjson STATIC
    third_party/cjson/cJSON.c
)
target_include_directories(cjson PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cjson
)
set_target_properties(cjson PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)
target_compile_options(cjson PRIVATE -w)

# ---------------------------------------------------------------------------
# 业务逻辑库
#
# 把逻辑放库里的好处: 主程序和单元测试链接同一份代码,
# 而且这一层可以交叉编译到 armhf (不依赖任何平台特有库)。
#
# 目录分工:
#   include/      对外头文件   (greeting.h / start.h / usetools.h / hardware_path.h / json_status.h / json_stream.h / serial_source.h)
#   src/library/  静态库实现   → libfirecontrol.a
#   src/source/   程序入口     (main.cpp)
#   src/appgui/   界面实现     (gui.cpp 真界面 / gui_stub.cpp 无界面版)
#
# 新增 .cpp 就在对应目标里登记 —— CMake 不自动扫描目录。
# 头文件不用单独登记: target_include_directories 已经把 include/ 公开出去。
# ---------------------------------------------------------------------------
add_library(firecontrol
    src/library/greeting.cpp
    src/library/usetools.cpp
    src/library/start.cpp
    src/library/json_stream.cpp
    src/library/json_status.cpp
    src/library/serial_source.cpp
)
target_include_directories(firecontrol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# cJSON 是实现细节 (json_status.h 不暴露 cJSON 类型), 所以 PRIVATE。
# 静态库的 PRIVATE 依赖会以 LINK_ONLY 形式传给最终可执行文件, 不会漏链。
target_link_libraries(firecontrol PRIVATE cjson)

# start.cpp 的蜂鸣器发声线程用 pthread。glibc 2.34+ 已经把 pthread 并进 libc,
# 但显式链接才不依赖那个细节(换 libc / 交叉工具链也稳)。
find_package(Threads REQUIRED)
target_link_libraries(firecontrol PUBLIC Threads::Threads)

# 版本字符串编进产物, 启动时打印 (板上不看文件名也能确认版本)。
target_compile_definitions(firecontrol PUBLIC "FIRE_VERSION_STRING=\"${FIRE_VERSION}\"")

# ---------------------------------------------------------------------------
# 可执行文件
# ---------------------------------------------------------------------------
add_executable(deffire-dev
    src/source/main.cpp
    src/appgui/gui_stub.cpp   # 同一套骨架的"无界面"实现: gui_build / render / gui_teardown
)
target_link_libraries(deffire-dev PRIVATE firecontrol)
# 产物名带 commit 次数 (FireFv0.<n>-console), 目标名保持稳定 —— 构建/脚本仍按
# --target deffire-dev 引用。
set_target_properties(deffire-dev PROPERTIES OUTPUT_NAME "${FIRE_CONSOLE_NAME}")

# ---------------------------------------------------------------------------
# armhf 产物的瘦身
#
# 必须在链接期加 -s。原因: 本机的 binutils 只编译了 x86 BFD 后端, 不认 ARM,
# 所以事后 strip 不了 (arm-linux-gnueabihf-strip 会报不支持); Zig 也没有
# strip 子命令。只能让链接器直接丢掉符号表。
# 不加这一条, Release 产物会带 debug_info, 大约 5.7 MB; 加了之后约 300 KB。
# ---------------------------------------------------------------------------
if(IS_CROSS_BUILD)
    target_link_options(deffire-dev PRIVATE -s)
    message(STATUS "armhf 优化: 链接期 -s 剥离符号 (本机无法事后 strip)")
endif()

# ---------------------------------------------------------------------------
# GUI 可执行文件 —— LVGL 图形界面 (deffire-gui-dev)
#
# 维护分工: 这一节的 LVGL 集成由 FBI-0537 负责; 上面的纯逻辑层与控制台程序
#           来自 Skywindfox, 改动时保留其注释与编译方式。
#
# 界面用 LVGL v9.2, 源码就在 third_party/lvgl (随仓库一起走, 不依赖网络)。
#
# 显示后端选的是 LVGL 自带的 X11 驱动, 而不是它更"嵌入式"的 framebuffer 驱动:
# 板上跑着 Xorg, /dev/fb0 归它管, 直接写 framebuffer 会和 Xorg 抢同一块屏。
# 中文靠 LVGL 的 FreeType 后端运行时加载板上的文泉驿正黑 —— 所以链接期需要
# libX11 + libfreetype (两者在板上都是现成的)。
#
#   WITH_GUI=AUTO (默认)
#       原生构建   → 找得到 x11/freetype2 就编
#       交叉编译   → 直接跳过 (裸交叉工具链里没有 X11/freetype 的 ARM 开发文件)
#
#   WITH_GUI=ON
#       强制尝试。交叉编译的 GUI **必须**走 armhf-toolchain 的 Docker 环境
#       (镜像里装了 armhf 版 libx11-dev / libfreetype-dev):
#           ./armhf-toolchain/build-armhf.sh        # Linux / macOS
#           .\armhf-toolchain\build-armhf.ps1       # Windows
#       不要手工拼 cmake 命令 —— 那两个脚本会带上正确的 toolchain 文件与
#       pkg-config 环境, 并给产物写工具链指纹。详见 armhf-toolchain/README.md
#
#       注: 仓库里的 cmake/toolchain-armhf.cmake (Zig 版) 已**废弃** —— 它没有
#       X11/freetype, 且 build.sh 里的 Zig 路径已删除, 现在没有任何脚本引用它。
#       留着只为查阅历史, 不要用它开 WITH_GUI=ON (会在下面直接报错)。
#
# 命名说明: CMake **目标名**固定是 deffire-dev / deffire-gui-dev (脚本按它引用);
#         **产物名**带 commit 次数: FireFv0.<n> 和 FireFv0.<n>-console (见文件开头的
#         版本号那一节)。
# ---------------------------------------------------------------------------
set(WITH_GUI "AUTO" CACHE STRING "是否构建 LVGL GUI (ON / OFF / AUTO)")
set_property(CACHE WITH_GUI PROPERTY STRINGS ON OFF AUTO)

if(WITH_GUI STREQUAL "AUTO")
    if(IS_CROSS_BUILD)
        set(_gui_attempt FALSE)
    else()
        set(_gui_attempt TRUE)
    endif()
elseif(WITH_GUI)
    set(_gui_attempt TRUE)
else()
    set(_gui_attempt FALSE)
endif()

if(NOT _gui_attempt)
    set(HAVE_GUI FALSE)
    if(IS_CROSS_BUILD)
        message(STATUS "GUI: 已跳过 —— 交叉编译默认不编 GUI (需要 WITH_GUI=ON 和 ARM 版 X11/freetype)")
    else()
        message(STATUS "GUI: 已跳过 (WITH_GUI=OFF)")
    endif()
else()
    # 用 pkg-config 而不是 CMake 的 FindX11:
    #   FindX11 只给出 X11 自己的头文件路径, 不给它传递依赖的路径
    #   (freetype2 的 ft2build.h 在 /usr/include/freetype2), 会编译失败。
    #   pkg-config 的 Requires 链能正确处理这些。
    find_package(PkgConfig QUIET)

    if(PkgConfig_FOUND)
        # x11 = LVGL 的 X11 显示/输入驱动; freetype2 = 中文字形
        pkg_check_modules(GUI_DEPS QUIET x11 freetype2)
    endif()

    if(GUI_DEPS_FOUND)
        # --- LVGL ---
        # LV_CONF_PATH 必须在 add_subdirectory 之前设置: lvgl 自己的 CMake 会把它
        # 变成编译宏 (-DLV_CONF_PATH=...)。找不到这个文件时 LVGL 会退回全默认配置
        # (16 位色、没开 X11/FreeType), 编出来的界面是另一回事。
        set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h")

        # 只编 LVGL 自己的 src/: 不编 examples/demos, 也不编内置的 ThorVG 矢量图库。
        # 用不到, 编了只是让构建变慢。
        set(LV_CONF_BUILD_DISABLE_EXAMPLES ON)
        set(LV_CONF_BUILD_DISABLE_DEMOS ON)
        set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON)

        set(LVGL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lvgl"
            CACHE PATH "LVGL 源码目录")
        if(NOT EXISTS "${LVGL_DIR}/lvgl.h")
            message(FATAL_ERROR
                "找不到 LVGL 源码: ${LVGL_DIR}/lvgl.h\n"
                "third_party/lvgl 是随仓库一起提交的, 检查是不是被清掉了")
        endif()
        add_subdirectory("${LVGL_DIR}" "${CMAKE_BINARY_DIR}/lvgl" EXCLUDE_FROM_ALL)

        # LVGL 是第三方代码, 不参与本项目"零警告"的要求。
        # 顺带把 x11/freetype 的头文件路径挂上去: LVGL 的 FreeType 后端要
        # ft2build.h (在 /usr/include/freetype2, 不是默认搜索路径), X11 后端要
        # X11/Xlib.h。少了这几行编译 lv_freetype.c 时会报 ft2build.h 找不到。
        target_compile_options(lvgl PRIVATE -w ${GUI_DEPS_CFLAGS_OTHER})
        target_include_directories(lvgl PRIVATE ${GUI_DEPS_INCLUDE_DIRS})

        set(HAVE_GUI TRUE)
        add_executable(deffire-gui-dev
            src/source/main.cpp    # 与控制台版同一个入口: main() 只调 cpp_start()
            src/appgui/gui.cpp     # 四个阶段里的 gui_build / render / gui_teardown
        )
        set_target_properties(deffire-gui-dev PROPERTIES OUTPUT_NAME "${FIRE_APP_NAME}")
        target_include_directories(deffire-gui-dev PRIVATE ${GUI_DEPS_INCLUDE_DIRS})
        target_compile_options(deffire-gui-dev PRIVATE ${GUI_DEPS_CFLAGS_OTHER})
        target_link_directories(deffire-gui-dev PRIVATE ${GUI_DEPS_LIBRARY_DIRS})
        target_link_libraries(deffire-gui-dev PRIVATE
            firecontrol                # 业务/硬件层: led_on / led_off 等
            lvgl::lvgl                 # LVGL 本体
            ${GUI_DEPS_LIBRARIES}      # libX11 + libfreetype
            pthread m                  # X11 后端用线程模拟 tick; LVGL 内部用 libm
        )

        # 交叉编译时同样要链接期 -s。
        # 本机无法事后 strip ARM (binutils 只编了 x86 BFD 后端), 所以瘦身只能
        # 在链接时做。
        if(IS_CROSS_BUILD)
            target_link_options(deffire-gui-dev PRIVATE -s)
        endif()

        message(STATUS "GUI: LVGL (third_party) + pkg-config ${GUI_DEPS_VERSION}")
        message(STATUS "     libs: ${GUI_DEPS_LIBRARIES}")
        message(STATUS "     incs: ${GUI_DEPS_INCLUDE_DIRS}")
    else()
        set(HAVE_GUI FALSE)
        set(_why "pkg-config 找不到 x11 / freetype2")
        if(IS_CROSS_BUILD)
            message(FATAL_ERROR
                "交叉编译启用了 GUI (WITH_GUI=ON) 但${_why}。\n"
                "\n"
                "交叉编译 GUI 必须用 armhf-toolchain 的 Docker 环境 —— 它在镜像里\n"
                "装了 armhf 版 X11/freetype, 并配好了 pkg-config 路径:\n"
                "\n"
                "    ./armhf-toolchain/build-armhf.sh        # Linux / macOS\n"
                "    .\\armhf-toolchain\\build-armhf.ps1       # Windows\n"
                "\n"
                "不要手工拼 cmake 命令: pkg-config 必须只查 armhf 的 .pc 文件\n"
                "(见 armhf-toolchain/toolchain-docker-armhf.cmake), 漏了会链到 x86-64。\n"
                "\n"
                "注: 仓库里那个 cmake/toolchain-armhf.cmake (Zig 版) 已废弃, 它没有\n"
                "X11/freetype, 不要拿它编 GUI。")
        else()
            message(WARNING
                "找不到 x11 / freetype2 开发文件, 跳过 GUI 目标。\n"
                "    Fedora:        sudo dnf install libX11-devel freetype-devel\n"
                "    Debian/Ubuntu: sudo apt install libx11-dev libfreetype-dev")
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# GPIO 输入读取 (libgpiod) —— 可选
#
# 板载 LED / 蜂鸣器走 sysfs (/sys/class/leds/*)，**不需要** libgpiod。
# 需要它的是接在 GPIO 上的开关量输入（火焰/人体/光电这类传感器），
# 由 useable_tools::gpio_read_value() 读取。
#
# 默认 AUTO: 有**可用版本**的 libgpiod 就编进去，否则明确说一声并跳过 ——
#   既不静默（那样"功能没编进去"会被当成"代码没写"），
#   也不因此让整个构建失败（开发机上没装 libgpiod-devel 时 GUI 仍然要能编）。
# 要强制要求就 -DWITH_GPIOD=ON，条件不满足直接 FATAL_ERROR。
#
# ⚠ **只支持 v1 API**（见 usetools.cpp）：v2 把那些符号全删了。
#     armhf 容器 / 板子: Debian 12 bookworm = 1.6.3 (v1) —— 可用
#     Fedora 44        : 2.2.5 (v2)                      —— **不可用**
#   所以原生 Fedora 构建即使装了 libgpiod-devel，也是"找到但版本不对 -> 不编入"；
#   想原生用就得先把 gpio_read_value() 移植到 v2 API。见 workflow.md §2.3。
#
# 依赖从哪来:
#   armhf 容器   : armhf-toolchain/Dockerfile 里装了 libgpiod-dev:armhf (1.6.3, v1)
#   Debian/Ubuntu: sudo apt install libgpiod-dev
#   Fedora       : sudo dnf install libgpiod-devel   (2.x = v2, 当前用不了)
# 板上跑的时候还需要运行时库: sudo apt install libgpiod2 (bookworm = 1.6.3, v1)
# 板上排查用的工具(gpiodetect / gpioinfo): sudo apt install gpiod
# ---------------------------------------------------------------------------
set(WITH_GPIOD "AUTO" CACHE STRING "是否编译 GPIO 读取 (ON / OFF / AUTO)")
set_property(CACHE WITH_GPIOD PROPERTY STRINGS ON OFF AUTO)

if(NOT WITH_GPIOD STREQUAL "OFF")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GPIOD_DEPS QUIET libgpiod)
    endif()

    # 版本闸门: 找到 v2 却硬编, 只会得到一堆"未声明标识符", 看起来像代码写错了。
    set(_gpiod_v1 FALSE)
    if(GPIOD_DEPS_FOUND AND GPIOD_DEPS_VERSION VERSION_LESS 2.0)
        set(_gpiod_v1 TRUE)
    endif()

    if(_gpiod_v1)
        # 宏必须是 PUBLIC: usetools.h 的声明与 usetools.cpp 的实现要看到同一个值,
        # 否则会出现"声明了但没实现"(或反过来)的链接期错误。
        target_compile_definitions(firecontrol PUBLIC FC_HAVE_GPIOD=1)
        target_include_directories(firecontrol PUBLIC ${GPIOD_DEPS_INCLUDE_DIRS})
        target_link_directories(firecontrol PUBLIC ${GPIOD_DEPS_LIBRARY_DIRS})
        target_link_libraries(firecontrol PUBLIC ${GPIOD_DEPS_LIBRARIES})

        message(STATUS "GPIO: libgpiod ${GPIOD_DEPS_VERSION} (v1) —— gpio_read_value() 已编入")
        message(STATUS "     libs: ${GPIOD_DEPS_LIBRARIES}")
    elseif(WITH_GPIOD STREQUAL "ON" AND GPIOD_DEPS_FOUND)
        message(FATAL_ERROR
            "WITH_GPIOD=ON 但找到的是 libgpiod ${GPIOD_DEPS_VERSION} (v2),\n"
            "而 gpio_read_value() 用的是 v1 API (gpiod_chip_open_by_label 等)。\n"
            "\n"
            "  - 交叉编译/板上: 用 armhf-toolchain 的容器, 里面是 1.6.3 (v1), 直接可用\n"
            "  - 原生 Fedora  : Fedora 44 只有 2.x, 要用得先把 gpio_read_value() 移植到 v2\n"
            "  - 或者: -DWITH_GPIOD=OFF 明确不要这个功能")
    elseif(WITH_GPIOD STREQUAL "ON")
        message(FATAL_ERROR
            "WITH_GPIOD=ON 但 pkg-config 找不到 libgpiod。\n"
            "\n"
            "    Debian/Ubuntu: sudo apt install libgpiod-dev\n"
            "    Fedora:        sudo dnf install libgpiod-devel   (注意 Fedora 44 是 v2, 用不了)\n"
            "\n"
            "交叉编译走 armhf-toolchain 的容器即可 —— 镜像里已装 libgpiod-dev:armhf,\n"
            "但**镜像需要重建**才会带上它:\n"
            "\n"
            "    ./armhf-toolchain/build-armhf.sh --rebuild")
    elseif(GPIOD_DEPS_FOUND)
        message(STATUS
            "GPIO: 找到的是 libgpiod ${GPIOD_DEPS_VERSION} (v2), 而代码用的是 v1 API\n"
            "      -> gpio_read_value() 不编入产物 (会返回 -1 并打印提示)。\n"
            "      交叉编译/板上用的是容器里的 1.6.3 (v1), 不受影响; 见 workflow.md §2.3")
    else()
        message(STATUS
            "GPIO: 未找到 libgpiod, gpio_read_value() 不编入产物 (会返回 -1 并打印提示)。\n"
            "      交叉编译/板上走容器(1.6.3, v1), 不受影响; 见 workflow.md §2.3")
    endif()
endif()

# ---------------------------------------------------------------------------
# 单元测试
# ---------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    if(IS_CROSS_BUILD)
        message(STATUS "测试: 已跳过 —— 交叉编译产物无法在构建机上执行")
    else()
        add_executable(test_greeting tests/test_greeting.cpp)
        target_link_libraries(test_greeting PRIVATE firecontrol)

        add_test(NAME greeting COMMAND test_greeting)

        add_executable(test_json_status tests/test_json_status.cpp)
        target_link_libraries(test_json_status PRIVATE firecontrol)

        add_test(NAME json_status COMMAND test_json_status)

        add_executable(test_json_stream tests/test_json_stream.cpp)
        target_link_libraries(test_json_stream PRIVATE firecontrol)

        add_test(NAME json_stream COMMAND test_json_stream)

        # 串口来源: 用伪终端冒充串口, 不需要真硬件 (Linux only)。
        add_executable(test_serial_source tests/test_serial_source.cpp)
        target_link_libraries(test_serial_source PRIVATE firecontrol)

        add_test(NAME serial_source COMMAND test_serial_source)
    endif()
endif()

# ---------------------------------------------------------------------------
# 工具链指纹 —— 产物隔离
#
# 同一个 deffire-gui-dev 可能来自 Docker / Zig / 板上原生编译, 三者编译器与 libc
# 都不同。把指纹写进构建目录, 有问题时第一眼就能确认来源; 指纹随产物一起
# 拷到板上, 板上也能查 (见 armhf-toolchain/verify-on-board.sh)。
#
# 如果构建目录里已有**不同**工具链的指纹, 说明在复用别人的构建目录, 给出警告。
# ---------------------------------------------------------------------------
set(_fp_file "${CMAKE_BINARY_DIR}/TOOLCHAIN.txt")
set(_fp_new "compiler   = ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}
compiler_path = ${CMAKE_CXX_COMPILER}
target     = ${CMAKE_SYSTEM_PROCESSOR}
cross      = ${IS_CROSS_BUILD}
gui        = ${HAVE_GUI}
cmake      = ${CMAKE_VERSION}
build_type = ${CMAKE_BUILD_TYPE}")

if(EXISTS "${_fp_file}")
    file(READ "${_fp_file}" _fp_old)
    # 比较前把首尾空白去掉: 写入时带了一个结尾换行 (见下面 file(WRITE ...)),
    # 而比较用的 _fp_new 没有 —— 直接 STREQUAL 永远不相等, 于是**每次
    # reconfigure 都报"指纹不一致"**。结果是"真的混了工具链"和"什么都没变"
    # 长得一模一样, 告警等于失效 (和 WARNING.md A 节那类误报同源)。
    string(STRIP "${_fp_old}" _fp_old)
    if(NOT _fp_old STREQUAL "${_fp_new}")
        message(WARNING
            "构建目录里的工具链指纹与当前配置不一致:\n"
            "    ${_fp_file}\n"
            "  可能在做交叉编译的目录里跑原生配置(或反之)。\n"
            "  建议先删掉构建目录再配置, 避免混用不同工具链的目标文件。")
    endif()
endif()
file(WRITE "${_fp_file}" "${_fp_new}\n")

# ---------------------------------------------------------------------------
# 构建信息
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "==================== 构建配置 ====================")
message(STATUS "  编译器      : ${CMAKE_CXX_COMPILER}")
message(STATUS "  编译器 ID   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  C++ 标准    : ${CMAKE_CXX_STANDARD}")
message(STATUS "  目标架构    : ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "  交叉编译    : ${IS_CROSS_BUILD}")
message(STATUS "  构建类型    : ${CMAKE_BUILD_TYPE}")
message(STATUS "  单元测试    : ${BUILD_TESTING}")
message(STATUS "  构建 GUI    : ${HAVE_GUI}")
message(STATUS "==================================================")
message(STATUS "")
