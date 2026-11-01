#!/usr/bin/env bash
# build.sh — FireControlApp 便捷构建
#
# 用法:
#   ./build.sh native      原生 x86-64: 配置 + 构建 + ctest
#   ./build.sh gui-full    原生 GUI, 全屏
#   ./build.sh gui-armhf   交叉编译 GUI (Docker, 需 docker/podman)
#   ./build.sh all         原生 + gui-armhf
#   ./build.sh clean       删除构建目录
#
# gui-armhf 后面的额外参数会**原样转给** armhf-toolchain/build-armhf.sh:
#   ./build.sh gui-armhf --rebuild     先重建环境镜像再编译
#   ./build.sh gui-armhf --shell       进容器交互

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

NATIVE_DIR="build"
ARMHF_DIR="build-armhf"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="$(nproc 2>/dev/null || echo 4)"

# 产物名带版本 (FireFv0.<commit 次数>)。优先读构建目录里的 FIRE_VERSION.txt
# (CMake 写的), 没有就用 git 数一次 —— 两边算法一致, 都是 commit 次数。
fire_name() {
    local dir="$1"
    if [ -f "$dir/FIRE_VERSION.txt" ]; then
        local name
        name="$(sed -n 's/^app=//p' "$dir/FIRE_VERSION.txt")"
        if [ -n "$name" ]; then
            printf '%s\n' "$name"
            return
        fi
    fi
    local count
    count="$(git -C "$ROOT" rev-list --count HEAD 2>/dev/null || echo 0)"
    printf 'FireFv0.%s\n' "$count"
}

fire_console_name() {
    local dir="$1"
    if [ -f "$dir/FIRE_VERSION.txt" ]; then
        local name
        name="$(sed -n 's/^console=//p' "$dir/FIRE_VERSION.txt")"
        if [ -n "$name" ]; then
            printf '%s\n' "$name"
            return
        fi
    fi
    printf '%s-console\n' "$(fire_name "$dir")"
}

c_ok()   { printf '\033[32m%s\033[0m\n' "$*"; }
c_info() { printf '\033[36m%s\033[0m\n' "$*"; }
c_err()  { printf '\033[31m%s\033[0m\n' "$*" >&2; }

need_native() {
    for t in cmake g++; do
        command -v "$t" >/dev/null 2>&1 || {
            c_err "✗ 缺少 $t"
            c_err "    sudo dnf install gcc gcc-c++ cmake ninja-build"
            exit 1
        }
    done
}

do_native() {
    c_info "======== 原生 x86-64 ========"
    need_native
    cmake -B "$NATIVE_DIR" -S . -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    cmake --build "$NATIVE_DIR" -j "$JOBS"

    c_info "-------- ctest --------"
    ctest --test-dir "$NATIVE_DIR" --output-on-failure

    c_ok "✓ 完成:"
    echo "    控制台程序 : ./$NATIVE_DIR/$(fire_console_name "$NATIVE_DIR")   (打印 hallo world)"
    echo "    图形界面   : ./$NATIVE_DIR/$(fire_name "$NATIVE_DIR")   (开窗口, 需 DISPLAY)"
    echo "    版本文件   : $NATIVE_DIR/FIRE_VERSION.txt"
}

do_gui_full() {
    c_info "======== 原生 GUI (x86-64, 全屏) ========"
    need_native

    cmake -B "$NATIVE_DIR" -S . -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    cmake --build "$NATIVE_DIR" -j "$JOBS" --target deffire-gui-dev

    if [[ -z "${DISPLAY:-}" ]]; then
        c_err "✗ DISPLAY 未设置, 开不了窗口"
        c_err "  在图形会话的终端里跑, 或先 export DISPLAY=:0"
        exit 1
    fi

    c_info "-------- 启动全屏窗口 --------"
    exec ./"$NATIVE_DIR"/"$(fire_name "$NATIVE_DIR")"
}

do_gui_armhf() {
    local script="$ROOT/armhf-toolchain/build-armhf.sh"

    if [ ! -x "$script" ]; then
        c_err "✗ 找不到 $script"
        exit 1
    fi

    if ! command -v docker >/dev/null 2>&1 && ! command -v podman >/dev/null 2>&1; then
        c_err "✗ 交叉编译 GUI 需要 docker 或 podman"
        c_err "    Fedora:  sudo dnf install podman"
        c_err "    其它:    https://docs.docker.com/get-docker/"
        c_err ""
        c_err "  为什么需要容器: GUI 依赖 armhf 版 X11/freetype, 这套环境由"
        c_err "  armhf-toolchain/Dockerfile 定义, 保证任何人构建结果一致。"
        c_err "  详见 armhf-toolchain/README.md"
        exit 1
    fi

    c_info "======== 交叉编译 GUI (armhf), 走 Docker ========"
    "$script" "$@"

    local exe="$ARMHF_DIR/$(fire_name "$ARMHF_DIR")"
    [ -f "$exe" ] || { c_err "✗ 没有产出 $exe"; exit 1; }

    if [ -f "$ARMHF_DIR/TOOLCHAIN.txt" ]; then
        echo
        c_info "-------- 工具链指纹 (产物隔离用) --------"
        sed 's/^/    /' "$ARMHF_DIR/TOOLCHAIN.txt"
    fi

    echo
    echo "    上板前校验:  ./armhf-toolchain/verify-on-board.sh fbi@<板子IP>"
}

do_clean() {
    rm -rf "$NATIVE_DIR" "$ARMHF_DIR" build-gui-armhf
    c_ok "✓ 已清理 $NATIVE_DIR $ARMHF_DIR build-gui-armhf(遗留)"
}

case "${1:-native}" in
    native)    do_native ;;
    gui-full)  do_gui_full ;;
    # ${@:2} 把 gui-armhf 之后的参数转给 armhf-toolchain/build-armhf.sh。
    # 早先这里写的是 `do_gui_armhf`(不传参), 而函数体里用 "$@" —— 于是
    # `./build.sh gui-armhf --rebuild` 里的 --rebuild 被**静默丢弃**,
    # 看起来像"重建了"其实没有。参数为空时 ${@:2} 展开为零个词, set -u 下也安全。
    gui-armhf) do_gui_armhf "${@:2}" ;;
    all)       do_native; echo; do_gui_armhf ;;
    clean)     do_clean ;;
    *)
        c_err "用法: $0 [native|gui-full|gui-armhf|all|clean]"
        exit 1
        ;;
esac
