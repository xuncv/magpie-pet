# 绿幕视频 → 桌宠图集
#
# 4 个视频（720x1280 / 24fps / 97 帧 / 4s）各对应一条动作轨：
#   idle 空闲 / think 思考 / work 工作 / done 完成
#
# 做法：
#   1. 涂掉右下角「豆包 AI 生成」水印（y>=1160 那块，角色脚底在 1073，涂掉不影响）
#   2. colorkey 抠掉背景（实测底色 #889D79，低饱和灰绿，不能用 chromakey）
#   3. 裁到 4 个视频的并集包围盒，再缩到单元格
#   4. 隔帧取样：97 帧 → 48 帧（12fps 采样，播放 4 秒，和原视频等速）
#
# 注意：**不要加 despill**。实测 type=green 会把蓝色头发抽成紫色/灰白。
#
# usage:
#   .\video-bake.ps1                       # 默认输出 pets\deepseek-girl
#   .\video-bake.ps1 -Cell 160x225         # 换尺寸
#   .\video-bake.ps1 -Every 1              # 不隔帧（97 帧，图集会很大）

param(
    [string]$OutDir = "pets\deepseek-girl",
    [string]$Cell = "128x179",
    [int]$Every = 2,
    [switch]$KeepFrames
)

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$ff = $env:FFMPEG
if (-not $ff) { $ff = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source }
if (-not $ff) { throw "找不到 ffmpeg：装到 PATH，或设置环境变量 FFMPEG=<ffmpeg 路径>" }

# 视频 → 轨道名 + 各自的底色（四个视频的绿不是一个值，逐个用）
$sources = @(
    @{ file = "e89e5167ccf1c67d42f47d03b4d707d1.mp4"; track = "idle";  key = "0x8FA65A" }  # 双手握拳站着
    @{ file = "cc1736929049ac56d1853a74410524f4.mp4"; track = "think"; key = "0x87A662" }  # 头顶 ? 气泡
    @{ file = "f1fa6082093c48dec11f217fbd18488d.mp4"; track = "work";  key = "0x8FAF5D" }  # 抱笔记本
    @{ file = "0840361e3bf266ccaa43b20e2c07c653.mp4"; track = "done";  key = "0x88AE63" }  # 举 ✓ 牌子
)

# 循环终点：AI 视频首尾不闭合，直接在视频里找一个跟首帧最像的帧当终点。
# 实测（f48→f0 vs 最佳点）：
#   idle 27.2（最佳 27.8，取满）  think 23.5→24.5（裁到 41）
#   work 20.3→27.5（裁到 46，+7.2dB）  done 27.6（取满）
$LOOPLEN = @{ idle = 49; think = 41; work = 46; done = 49 }

# 抠图参数（实测值）
$SIM    = "0.16"      # 相似度：再大就开始吃角色颜色
$BLEND  = "0.01"      # alpha 斜坡要短，不然边缘一圈半透明会很脏
# 收尾：去掉边缘残留的绿，并把很淡的 alpha 压掉（防止描边一圈发脏）
$POST   = "geq=r='r(X,Y)':g='if(gt(g(X,Y),max(r(X,Y),b(X,Y))),max(r(X,Y),b(X,Y)),g(X,Y))':b='b(X,Y)':a='clip((alpha(X,Y)-25)*255/200,0,255)'"
# 4 个视频的并集包围盒（alpha>25 实测）是 672x912 @ (18,170)。
# done 的对勾牌会吊到 x=18，之前从 28 开始裁，把牌子左边切平了。
# 源视频本身左边只有 18px 可甩，所以直接 x0=0，把余量都用上：
# 704x984 @ (0,133)（宽高比 0.7154 ≈ 单元格 128:179）
$CROP   = "704:984:0:133"
# 源视频左边只有 18px 可甩，直接裁完缩到 128 格只剩 3px 边距，
# 对勾牌吊到最左那几帧看着像贴边/出画。再补一圈透明内边距（居中），
# 相当于把内容缩小一点换出边框：760x1063 @ (28,39) → 边距 ≈ 8px
$PAD    = "760:1063:28:39"

$cw, $ch = $Cell -split "x"
$cw = [int]$cw; $ch = [int]$ch
$outPath = Join-Path $root $OutDir
$framesRoot = Join-Path $env:TEMP "petvid-frames"

New-Item -ItemType Directory -Force $outPath | Out-Null
Remove-Item $framesRoot -Recurse -Force -ErrorAction SilentlyContinue

$rows = @()
foreach ($s in $sources) {
    $file = $s.file
    $track = $s.track
    $key = $s.key
    $src = Join-Path $root (Join-Path "sources\video" $file)
    if (-not (Test-Path $src)) { $src = Join-Path $root $file }
    if (-not (Test-Path $src)) { throw "找不到视频 $file（放到 sources\video\）" }

    $dir = Join-Path $framesRoot $track
    New-Item -ItemType Directory -Force $dir | Out-Null

    $sel = if ($Every -gt 1) { "select='not(mod(n\,$Every))'" } else { "null" }
    # 水印用同色涂掉，再 colorkey
    $wm = "drawbox=x=460:y=1160:w=260:h=120:color=${key}:t=fill"
    $vf = "$wm,colorkey=${key}:${SIM}:${BLEND},crop=${CROP},format=rgba,pad=${PAD}:color=black@0,scale=${cw}:${ch}:flags=lanczos,${sel},${POST}"

    Write-Host ("[{0}] 抠图（key {1}）+ 裁切 + 缩放到 {2}x{3} ..." -f $track, $key, $cw, $ch)
    & $ff -nostdin -y -loglevel error -i $src -vf $vf -vsync 0 (Join-Path $dir "%03d.png")
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg 处理 $track 失败" }

    $n = @(Get-ChildItem $dir -Filter *.png).Count
    $keep = if ($LOOPLEN.ContainsKey($track)) { [Math]::Min($LOOPLEN[$track], $n) } else { $n }
    if ($keep -lt $n) {
        Get-ChildItem $dir -Filter *.png | Sort-Object Name | Select-Object -Skip $keep | Remove-Item -Force
        $n = $keep
    }
    Write-Host ("[{0}] {1} 帧（循环长 {2}）" -f $track, $n, $keep)
    $rows += @{ track = $track; dir = $dir; frames = $n }
}

# 每行拼成一张长条；短的轨用透明补齐到 cols，再纵向拼成图集
$cols = ($rows | ForEach-Object { $_.frames } | Measure-Object -Maximum).Maximum
$rowFiles = @()
foreach ($r in $rows) {
    $rowPng = Join-Path $framesRoot ("row-" + $r.track + ".png")
    $vf = "tile={0}x1,format=rgba,pad={1}:{2}:0:0:color=black@0" -f $r.frames, ($cols * $cw), $ch
    & $ff -nostdin -y -loglevel error -framerate 1 -i (Join-Path $r.dir "%03d.png") `
        -vf $vf -frames:v 1 $rowPng
    if ($LASTEXITCODE -ne 0) { throw "拼行失败: $($r.track)" }
    $rowFiles += $rowPng
}

$args = @()
foreach ($f in $rowFiles) { $args += @("-i", $f) }
$stack = ($rowFiles | ForEach-Object { "[$([array]::IndexOf($rowFiles, $_)):v]" }) -join ""
$sheet = Join-Path $outPath "spritesheet.png"
& $ff -nostdin -y -loglevel error @args -filter_complex "${stack}vstack=inputs=$($rowFiles.Count)" $sheet
if ($LASTEXITCODE -ne 0) { throw "纵向拼接失败" }

# layout.txt
$ms = [int](1000 * 4.04)      # 视频 4.04 秒
$lines = @("cell=${cw}x${ch}")
$lines += "cols=$cols"
$lines += "rows=$($rows.Count)"
for ($i = 0; $i -lt $rows.Count; $i++) {
    $t = $rows[$i]
    $name = switch ($t.track) {
        "think" { "review" }     # 思考 → 桌宠的 review 轨
        "work"  { "running" }    # 工作 → running 轨
        "done"  { "jumping" }    # 完成 → jumping 轨
        default { $t.track }
    }
    $lines += ("track={0} row={1} frames={2} total={3}" -f $name, $i, $t.frames, $ms)
}
# waiting 复用 idle（素材只有 4 种状态）
$lines += ("track=waiting row=0 frames={0} total={1}" -f $rows[0].frames, $ms)
Set-Content (Join-Path $outPath "layout.txt") $lines -Encoding ASCII

if (-not $KeepFrames) {
    # 先出 GIF 预览（透明底在浏览器/图片查看器里容易发灰，直接贴一层浅灰底）
    $gifDir = Join-Path $outPath "preview"
    New-Item -ItemType Directory -Force $gifDir | Out-Null
    $fps = [math]::Round(1000.0 / ($ms / $cols), 2)
    foreach ($r in $rows) {
        $gif = Join-Path $gifDir ($r.track + ".gif")
        & $ff -nostdin -y -loglevel error -framerate $fps -i (Join-Path $r.dir "%03d.png") `
            -vf "split[a][b];[a]palettegen=reserve_transparent=1:stats_mode=full[p];[b][p]paletteuse=alpha_threshold=128" `
            -loop 0 $gif
        if ($LASTEXITCODE -eq 0) { Write-Host ("  预览 {0}.gif" -f $r.track) }
    }
    Remove-Item $framesRoot -Recurse -Force -ErrorAction SilentlyContinue
}

$kb = [math]::Round((Get-Item $sheet).Length / 1KB)
Write-Host ""
Write-Host ("图集: {0}  ({1} KB, {2} 列 x {3} 行, 单元格 {4}x{5})" -f $sheet, $kb, $cols, $rows.Count, $cw, $ch) -ForegroundColor Green
Get-Content (Join-Path $outPath "layout.txt") | ForEach-Object { Write-Host "  $_" }
