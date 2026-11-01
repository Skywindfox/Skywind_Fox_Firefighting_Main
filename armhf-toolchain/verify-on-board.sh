#!/usr/bin/env bash
# verify-on-board.sh — 在目标板上验证 armhf 产物是否与本板匹配
#
# 为什么需要它: 同一个产物名 (FireFv0.<n>) 可能来自三套工具链
#     a) Docker (Debian bookworm + gcc)     ← 现在统一到这套
#     b) Zig 0.16 (开发机, 手工 sysroot)    ← 旧路径
#     c) 板上原生编译 (g++)                 ← 最保险
#   三者编译器与 libc 都不同, 出问题时第一步就是确认"这个二进制是谁编的、
#   它要的库本板有没有"。
#
# 用法 (在开发机上跑, 通过 ssh 到板子检查):
#   ./armhf-toolchain/verify-on-board.sh fbi@192.168.1.100
#   ./armhf-toolchain/verify-on-board.sh fbi@板子IP build-armhf/FireFv0.<n>
#
# 也可以把本脚本拷到板上直接跑 (板上统一用 ~/Desktop):
#   scp verify-on-board.sh 板子:~/Desktop/
#   ssh 板子 'cd ~/Desktop && bash verify-on-board.sh ./FireFv0.<n> ./TOOLCHAIN.txt'
#   (第一个参数若是"存在的文件", 自动进入就地检查模式 —— 不必手写空串)
#   等价写法: bash verify-on-board.sh "" ./FireFv0.<n> ./TOOLCHAIN.txt

set -uo pipefail

HOST="${1:-}"
# 本地产物名带 commit 次数: 以 build-armhf/FIRE_VERSION.txt 为准, 没有就数 git。
_local_default() {
    if [ -f build-armhf/FIRE_VERSION.txt ]; then
        local n
        n="$(sed -n 's/^app=//p' build-armhf/FIRE_VERSION.txt)"
        [ -n "$n" ] && { printf 'build-armhf/%s\n' "$n"; return; }
    fi
    printf 'build-armhf/FireFv0.%s\n' "$(git rev-list --count HEAD 2>/dev/null || echo 0)"
}
LOCAL_EXE="${2:-$(_local_default)}"
# 远端临时目录: 用 ~ 而不是 /root, 这样以 fbi 用户 ssh 时也能写 (板上的普通用户
# 是 fbi, 不是 root)。~ 在双引号里不会被本地 shell 展开, 由远端 shell 展开。
REMOTE_DIR="~/Desktop/fc-verify"

c_info() { printf '\033[36m%s\033[0m\n' "$*"; }
c_ok()   { printf '\033[32m%s\033[0m\n' "$*"; }
c_warn() { printf '\033[33m%s\033[0m\n' "$*"; }
c_err()  { printf '\033[31m%s\033[0m\n' "$*" >&2; }

# ---------------------------------------------------------------------------
# 在板上执行的检查逻辑（会作为远程脚本传入）
# ---------------------------------------------------------------------------
read -r -d '' REMOTE_CHECK <<'REMOTE' || true
#!/bin/bash
# 产物名带版本 (FireFv0.<commit 次数>); 正常调用会显式传进来。
EXE="${1:-./FireFv0.0}"
FP="${2:-}"
fail=0

# --- 依赖工具自检 -----------------------------------------------------------
# 缺工具时后面的检查会**静默跳过**, 然后仍然打印"与本板匹配"。
# 最危险的是"运行时依赖是否齐全": readelf 不在时它一个库都不查, 结果是空的,
# 看着就像"全都没问题"。所以缺工具必须在这里显式失败, 宁可误报也不能漏报。
READELF=""
for c in readelf arm-linux-gnueabihf-readelf; do
    if command -v "$c" >/dev/null 2>&1; then READELF="$c"; break; fi
done

for tool in file ldconfig; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "  ✗ 板上缺 $tool —— 检查结果不可靠"
        echo "      装上: apt install $tool"
        fail=1
    fi
done
if [ -z "$READELF" ]; then
    echo "  ✗ 板上缺 readelf —— 无法检查 ABI 与运行时依赖"
    echo "      装上: apt install binutils"
    fail=1
fi

echo "  --- 板子环境 ---"
echo "    架构      : $(uname -m)"
echo "    内核      : $(uname -r)"
[ -f /etc/os-release ] && echo "    系统      : $(. /etc/os-release && echo "$PRETTY_NAME")"
echo "    glibc     : $(ldd --version 2>/dev/null | head -1)"
echo "    内存      : $(awk '/MemTotal/{printf "%.0f MiB", $2/1024}' /proc/meminfo)"

echo
echo "  --- 产物基本信息 ---"
if [ ! -f "$EXE" ]; then echo "    ✗ 找不到 $EXE"; exit 1; fi
printf '    体积      : %s 字节\n' "$(stat -c %s "$EXE")"
file "$EXE" 2>/dev/null | sed 's/^/    /'

echo
echo "  --- 架构与 ABI 匹配 ---"
if file "$EXE" 2>/dev/null | grep -q 'ELF 32-bit'; then
    echo "    ✓ 32 位 ELF"
else
    echo "    ✗ 不是 32 位 ELF（板子是 armv7l，必须 32 位）"; fail=1
fi
if file "$EXE" 2>/dev/null | grep -qi 'ARM'; then
    echo "    ✓ ARM 架构"
else
    echo "    ✗ 不是 ARM（可能是 x86-64 误传上来了）"; fail=1
fi
if [ -n "$READELF" ] && LC_ALL=C "$READELF" -h "$EXE" 2>/dev/null | grep -qi 'hard-float'; then
    echo "    ✓ 硬浮点 ABI (ELF header Flags)"
elif [ -n "$READELF" ] && LC_ALL=C "$READELF" -A "$EXE" 2>/dev/null | grep -qiE 'Tag_ABI_VFP_args.*VFP'; then
    echo "    ✓ 硬浮点 ABI (.ARM.attributes)"
else
    echo "    ⚠ 没检出硬浮点标记（板子是 hard-float，可能跑不起来）"
fi

echo
echo "  --- 运行时依赖是否齐全 ---"
missing=0
for lib in $(LC_ALL=C "$READELF" -d "$EXE" 2>/dev/null | awk '/NEEDED/{gsub(/[][]/,"");print $NF}'); do
    if ldconfig -p 2>/dev/null | grep -q "$lib"; then
        echo "    ✓ $lib"
    else
        # 再查常见路径，避免 ldconfig 缓存不全导致误报
        if find /lib /usr/lib -name "$lib" 2>/dev/null | head -1 | grep -q .; then
            echo "    ✓ $lib (在文件系统里找到)"
        else
            echo "    ✗ $lib  缺失!"; missing=1; fail=1
        fi
    fi
done

echo
echo "  --- 中文字体 ---"
found_font=""
for f in /usr/share/fonts/truetype/wqy/wqy-zenhei.ttc \
         /usr/share/fonts/wqy-zenhei/wqy-zenhei.ttc \
         /usr/share/fonts/truetype/wqy/wqy-microhei.ttc \
         /usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc \
         /usr/share/fonts/truetype/arphic/uming.ttc ; do
    if [ -f "$f" ]; then echo "    ✓ $f"; found_font="$f"; break; fi
done
if [ -z "$found_font" ]; then
    echo "    ⚠ 没找到常见中文字体（界面汉字会显示为方框）"
    echo "      装一个: apt install fonts-wqy-zenhei"
fi

echo
echo "  --- X server ---"
if [ -S /tmp/.X11-unix/X0 ]; then echo "    ✓ /tmp/.X11-unix/X0 存在"; else echo "    ⚠ 没看到 X0 socket"; fi
echo "    DISPLAY   : ${DISPLAY:-<未设置>}"

# 工具链指纹: 只读文本文件。
# 必须判可读 + 非二进制 —— 传错路径(比如把 ELF 当指纹文件传进来)时,
# 直接 sed 会把整个二进制打到终端上, 输出刷屏且毫无意义。
if [ -n "$FP" ] && [ -f "$FP" ] && [ -r "$FP" ] && [ -s "$FP" ] \
   && ! LC_ALL=C grep -qP '[\x00-\x08\x0e-\x1f]' "$FP" 2>/dev/null; then
    echo
    echo "  --- 该产物的工具链指纹 ---"
    sed 's/^/    /' "$FP"
elif [ -n "$FP" ]; then
    echo
    echo "  --- 工具链指纹 ---"
    echo "    ⚠ 跳过: $FP 不存在/不可读/是二进制"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "  ==== 结论: 产物与本板匹配, 可以运行 ===="
    echo "       DISPLAY=:0 $EXE"
else
    echo "  ==== 结论: 有问题, 见上面的 ✗ 项 ===="
fi
exit $fail
REMOTE

# ---------------------------------------------------------------------------
# 在板上就地跑 → 直接执行检查, 不走 ssh
#
#   两种写法都支持:
#     verify-on-board.sh ./FireFv0.<n> ./TOOLCHAIN.txt
#     verify-on-board.sh "" ./FireFv0.<n> ./TOOLCHAIN.txt
#   前一种的 $1 是个**存在的文件**, 说明它不是要 ssh 的目标主机 ——
#   否则会去 ssh 一个叫 "./FireFv0.<n>" 的主机名而失败。
# ---------------------------------------------------------------------------
FP_ARG="${3:-}"
if [ -n "$HOST" ] && [ -e "$HOST" ]; then
    LOCAL_EXE="$HOST"
    FP_ARG="${2:-}"
    HOST=""
fi

if [ -z "$HOST" ]; then
    c_info "=== 本地(on-board)模式 ==="
    exec bash -c "$REMOTE_CHECK" _ "$LOCAL_EXE" "$FP_ARG"
fi

# ---------------------------------------------------------------------------
# 带 HOST → 通过 ssh 上传并检查
# ---------------------------------------------------------------------------
c_info "=== 检查 $HOST ==="

if [ ! -f "$LOCAL_EXE" ]; then
    c_err "✗ 本地找不到产物: $LOCAL_EXE"
    c_err "  先构建: ./armhf-toolchain/build-armhf.sh"
    exit 1
fi

ssh -o ConnectTimeout=10 "$HOST" "mkdir -p $REMOTE_DIR" || { c_err "✗ 连不上 $HOST"; exit 1; }

c_info "上传产物与指纹..."
scp -q "$LOCAL_EXE" "$HOST:$REMOTE_DIR/$(basename "$LOCAL_EXE")"
FP_LOCAL="$(dirname "$LOCAL_EXE")/TOOLCHAIN.txt"
if [ -f "$FP_LOCAL" ]; then
    scp -q "$FP_LOCAL" "$HOST:$REMOTE_DIR/TOOLCHAIN.txt"
fi

ssh "$HOST" "cat > $REMOTE_DIR/check.sh" <<< "$REMOTE_CHECK"

echo
ssh "$HOST" "bash $REMOTE_DIR/check.sh $REMOTE_DIR/$(basename "$LOCAL_EXE") $REMOTE_DIR/TOOLCHAIN.txt"
rc=$?
echo
if [ $rc -eq 0 ]; then
    c_ok "✓ 验证通过"
else
    c_err "✗ 验证失败（见上面的 ✗ 项）"
fi
exit $rc
