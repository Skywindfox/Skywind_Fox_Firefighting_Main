# build-armhf.ps1 — 用 Docker 交叉编译 FireControlApp 到 armhf (Windows)
#
# 用法 (PowerShell, 在仓库根目录或任意位置):
#   .\armhf-toolchain\build-armhf.ps1              构建
#   .\armhf-toolchain\build-armhf.ps1 -Rebuild     先重建镜像再构建
#   .\armhf-toolchain\build-armhf.ps1 -Shell       进容器交互 (调试工具链用)
#
# 需要 Docker Desktop 正在运行。
# 产物在 build-armhf\, 其中 TOOLCHAIN.txt 记录工具链指纹(见文件末尾说明)。
#
# 访问不了 docker.io 时用镜像站覆盖基础镜像:
#   .\armhf-toolchain\build-armhf.ps1 -BaseImage docker.m.daocloud.io/library/debian:bookworm-slim

[CmdletBinding()]
param(
    [switch]$Rebuild,
    [switch]$Shell,
    [string]$Image = "firecontrol-armhf:bookworm",
    # 环境镜像的基础镜像。留空 = 用 Dockerfile 里 `ARG BASE=` 的默认值。
    [string]$BaseImage = ""
)

# PowerShell 5.1 的坑: 原生命令(docker / cmake)往 stderr 写任何东西 —— 构建进度、
# pull 的层信息、gcc 警告 —— 在 $ErrorActionPreference='Stop' 下都会被当成终止
# 错误, 于是第一次 docker build 就会莫名其妙中断。
# 所以这里用 Continue, 成败一律看 $LASTEXITCODE（下面每处都检查了）。
$ErrorActionPreference = "Continue"

function Info($m) { Write-Host $m -ForegroundColor Cyan }
function Ok($m)   { Write-Host $m -ForegroundColor Green }
function Fail($m) { Write-Host $m -ForegroundColor Red }

# --- 仓库根目录 = 本脚本所在目录的上一级 ---
$Here       = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $Here
$BuildDir   = Join-Path $ProjectRoot "build-armhf"

# 版本号 = commit 次数 (产物名 FireFv0.<n> / FireFv0.<n>-console)。
# .dockerignore 把 .git 排除了, 容器里数不出来, 所以这里数好再 --build-arg 传进去;
# 没有 git 时退回 0 (产物名仍可用, 只是不能唯一对应提交)。
$RevCount = 0
try {
    $countText = (& git -C $ProjectRoot rev-list --count HEAD 2>$null)
    if ($countText -match '^\d+$') { $RevCount = [int]$countText }
} catch { }
$GuiName    = "FireFv0.$RevCount"
$ConsoleName = "$GuiName-console"

# 两个目标由同一次 cmake 产出, 少任何一个都说明构建不完整 —— 清单只写这一处。
$Artifacts  = @($GuiName, $ConsoleName)

# --- 检查 docker ---
if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    Fail "找不到 docker。请安装并启动 Docker Desktop:"
    Fail "    https://docs.docker.com/desktop/install/windows-install/"
    exit 1
}

$null = & docker info 2>&1
if ($LASTEXITCODE -ne 0) {
    Fail "Docker 没有在运行。启动 Docker Desktop 后重试。"
    exit 1
}

# --- 镜像 ---
$imageExists = $true
$null = & docker image inspect $Image 2>&1
if ($LASTEXITCODE -ne 0) { $imageExists = $false }

if ($Rebuild -or (-not $imageExists)) {
    if ($Rebuild) { Info "=== 重建镜像 $Image ===" }
    else          { Info "=== 镜像 $Image 不存在, 开始构建 ===" }
    Info "    (首次较慢; 之后除非 -Rebuild 否则复用)"

    # 基础镜像名不能从 `FROM` 行取 —— Dockerfile 里写的是
    #     ARG BASE=debian:bookworm-slim
    #     FROM ${BASE}
    # `FROM` 的第二个字段是字面量 "${BASE}", 拿去 pull 会去拉一个叫 "${BASE}"
    # 的镜像, 必然失败。真正的名字在 `ARG BASE=` 的默认值里。
    if ([string]::IsNullOrWhiteSpace($BaseImage)) {
        $df = Get-Content (Join-Path $Here "Dockerfile")
        foreach ($line in $df) {
            if ($line -match '^\s*ARG\s+BASE\s*=\s*(.+?)\s*$') {
                $BaseImage = $Matches[1].Trim('"').Trim("'")
                break
            }
        }
    }

    if ([string]::IsNullOrWhiteSpace($BaseImage)) {
        Fail "✗ 无法从 Dockerfile 解析基础镜像名, 请用 -BaseImage ... 显式指定"
        exit 1
    }

    Info "    基础镜像: $BaseImage"
    & docker build --build-arg "BASE=$BaseImage" -t $Image $Here
    if ($LASTEXITCODE -ne 0) { Fail "镜像构建失败"; exit 1 }
}

# --- 交互模式 ---
if ($Shell) {
    Info "=== 进入容器 ($Image) ==="
    & docker run --rm -it -v "${ProjectRoot}:/work" -w /work $Image /bin/bash
    exit $LASTEXITCODE
}

# --- 构建 ---
# 用 Dockerfile.build-armhf + COPY, 而不是 `-v` 挂载源码。
# 挂载在部分环境会是只读(详见 build-armhf.sh 里的说明); COPY 不依赖挂载, 任何环境都能构建。
if (-not (Test-Path $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir | Out-Null }

$BuildImage = "firecontrol-armhf-build:tmp"

Info "=== 容器内交叉编译 (docker) ==="
Info "    方式: COPY 源码进镜像层 (不依赖 bind mount)"

& docker build `
    --build-arg "BASE_IMAGE=$Image" `
    --build-arg "FIRE_VERSION_COUNT=$RevCount" `
    -f "$Here/Dockerfile.build-armhf" `
    -t $BuildImage `
    $ProjectRoot
if ($LASTEXITCODE -ne 0) { Fail "容器内构建失败"; exit 1 }

# --- 取回产物 ---
# docker 没有"从镜像直接拷文件"的命令, 标准做法: create 一个容器再 cp (create 不启动, 很快)
Info "=== 取回产物 ==="

# 先清掉上一次的产物。不清的话, "新产物缺失"会被同名旧文件顶替, 下面的逐个检查
# 就形同虚设 —— 产物改名那次就出现过 halloworld-* 与 deffire-* 并存的情况。
# 删不掉(被编辑器/杀毒占用)直接失败: 留着旧文件比构建失败更危险。
# 改名前的旧产物 (deffire-*) 顺手清掉, 防止被误拷。
foreach ($legacy in @("deffire-gui-dev", "deffire-dev")) {
    $old = Join-Path $BuildDir $legacy
    if (Test-Path $old) { Remove-Item $old -Force -ErrorAction SilentlyContinue; Write-Host "  已清掉旧名字产物 $legacy" }
}

foreach ($exe in $Artifacts) {
    $old = Join-Path $BuildDir $exe
    if (Test-Path $old) {
        Remove-Item $old -Force -ErrorAction SilentlyContinue
        if (Test-Path $old) {
            Fail "旧产物删不掉: $old"
            Fail "    (多半被其它进程占用, 关掉编辑器 / 暂停同步盘再试)"
            exit 1
        }
        Write-Host "  已清掉旧产物 $exe"
    }
}

$cid = (& docker create $BuildImage).Trim()
foreach ($exe in $Artifacts) {
    & docker cp "${cid}:/work/build-armhf/$exe" (Join-Path $BuildDir $exe) 2>$null
    if ($LASTEXITCODE -eq 0) { Write-Host "  ✓ $exe" }
    else { Write-Host "  ✗ $exe  (docker cp 失败, 退出码 $LASTEXITCODE)" }
}
# 版本清单 (build.sh / deploy 靠它拿实际产物名), 取不到不算失败。
& docker cp "${cid}:/work/build-armhf/FIRE_VERSION.txt" (Join-Path $BuildDir "FIRE_VERSION.txt") 2>$null | Out-Null
if ($LASTEXITCODE -eq 0) { Write-Host "  ✓ FIRE_VERSION.txt" }
else { Write-Host "  ! FIRE_VERSION.txt 没取到 (脚本会退回数 git)" }

& docker rm $cid | Out-Null
& docker rmi $BuildImage 2>$null | Out-Null

# --- 产物指纹 ---
Info "=== 写工具链指纹 ==="
$fp = & docker run --rm $Image cat /etc/firecontrol-toolchain.txt
$stamp = @(
    "# 本目录产物由以下工具链生成 —— 出问题时先看这里确认来源"
    $fp
    "build_mode   = copy (容器内编译, 不依赖 bind mount)"
    "host_runtime = docker-desktop (windows)"
    "built_local  = $((Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))"
)
# 不用 Set-Content -Encoding UTF8: PowerShell 5.1 会写成"带 BOM 的 UTF-8"且用 CRLF。
# 这个文件要跟着产物上 Linux / 板子, 所以显式写成 无 BOM + LF。
[System.IO.File]::WriteAllText(
    (Join-Path $BuildDir "TOOLCHAIN.txt"),
    ($stamp -join "`n") + "`n",
    (New-Object System.Text.UTF8Encoding $false))
$stamp | ForEach-Object { "    $_" }

# --- 结果 ---
Write-Host ""
Info "=== 产物 ==="
# 逐个检查, 而不是"至少有一个": 两个目标由同一次 cmake 产出, 少任何一个都说明
# 构建不完整。原来的写法只判断"一个都没有", 于是 GUI 缺失、控制台在时仍然算通过
# —— 在 CI 里等于静默交付了一个没有界面的构建。
$missing = @()
foreach ($exe in $Artifacts) {
    $f = Join-Path $BuildDir $exe
    if (Test-Path $f) {
        $sz = (Get-Item $f).Length
        Write-Host ("  {0,-20} {1,9} 字节" -f $exe, $sz)
    } else {
        Write-Host ("  {0,-20} 缺失" -f $exe)
        $missing += $exe
    }
}
if ($missing.Count -gt 0) {
    Fail "缺产物: $($missing -join ', ')"
    exit 1
}

Write-Host ""
Ok "✓ 完成: $BuildDir"
Write-Host @"

  部署到板子 (板上统一用 ~/Desktop):
      scp build-armhf/$GuiName build-armhf/TOOLCHAIN.txt fbi@<板子IP>:~/Desktop/
      ssh -t fbi@<板子IP> 'cd ~/Desktop && DISPLAY=:0 ./$GuiName'

  板上自检(确认产物与板子匹配):
      在 Linux / WSL / Git Bash 里跑:
      bash armhf-toolchain/verify-on-board.sh fbi@<板子IP>
"@
